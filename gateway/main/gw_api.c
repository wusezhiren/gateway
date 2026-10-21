#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "gw_api.h"
#include "gw_cloud.h"
#include "gw_http.h"
#include "gw_link.h"
#include "gw_ota.h"
#include "gw_prov.h"
#include "gw_wifi.h"

static const char *TAG = "gw_api";

#define BIND_TIMEOUT_US   (60 * 1000000LL)  /* 等待屏幕确认 */
#define BIND_KEEP_US      (30 * 1000000LL)  /* 结果保留，供 App 轮询取走 */
#define BODY_MAX          256

typedef enum { BIND_NONE, BIND_PENDING, BIND_ALLOWED, BIND_DENIED } bind_state_t;

static struct {
    bind_state_t state;
    uint32_t id;
    char name[GW_API_NAME_LEN + 1];
    int64_t since_us;
} s_bind;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_api_bind_cb_t s_bind_cb;

/* ---------- 工具 ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* 读 JSON 请求体；失败时已回复错误，返回 NULL */
static cJSON *recv_json(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        gw_http_error(req, "400 Bad Request", "bad_body");
        return NULL;
    }
    char body[BODY_MAX + 1];
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            gw_http_error(req, "400 Bad Request", "recv_failed");
            return NULL;
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *o = cJSON_Parse(body);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        gw_http_error(req, "400 Bad Request", "bad_body");
        return NULL;
    }
    return o;
}

static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

static const char *ch_type_name(uint8_t t)
{
    switch (t) {
    case MP_CH_RELAY: return "relay";
    case MP_CH_PWM:   return "pwm";
    default:          return "unknown";
    }
}

static const char *chip_name(uint8_t chip)
{
    switch (chip) {
    case CHIP_ESP32C3: return "esp32c3";
    case CHIP_ESP32S3: return "esp32s3";
    case CHIP_ESP32C6: return "esp32c6";
    default: return "unknown";
    }
}

/* 节点表放静态区，避免占调用方栈；HTTP 与云端两个任务都会用，加锁 */
static gw_node_t s_nodes[GW_MAX_NODES];
static SemaphoreHandle_t s_nodes_mtx;

#define NODES_LOCK()   xSemaphoreTake(s_nodes_mtx, portMAX_DELAY)
#define NODES_UNLOCK() xSemaphoreGive(s_nodes_mtx)

static void add_nodes(cJSON *o)
{
    int64_t now = esp_timer_get_time();
    cJSON_AddNumberToObject(o, "pair_left", gw_link_pair_remaining());
    cJSON *arr = cJSON_AddArrayToObject(o, "nodes");
    for (int i = 0; i < GW_MAX_NODES; i++) {
        const gw_node_t *n = &s_nodes[i];
        if (!n->used) {
            continue;
        }
        char mac[18];
        snprintf(mac, sizeof(mac), MACSTR, MAC2STR(n->mac));
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "mac", mac);
        cJSON_AddStringToObject(j, "name", n->name);
        cJSON_AddBoolToObject(j, "online", n->online);
        cJSON_AddNumberToObject(j, "rssi", n->rssi);
        cJSON_AddNumberToObject(j, "uptime_s", n->uptime_s);
        cJSON_AddNumberToObject(j, "last_seen_s",
                                n->last_seen_us ? (double)((now - n->last_seen_us) / 1000000) : -1);
        cJSON_AddStringToObject(j, "version", n->fw_version);
        cJSON_AddStringToObject(j, "project", n->project);
        cJSON_AddStringToObject(j, "chip", chip_name(n->chip));
        cJSON *chs = cJSON_AddArrayToObject(j, "ch");
        for (int c = 0; c < n->n_ch && c < MP_MAX_CH; c++) {
            cJSON *ch = cJSON_CreateObject();
            cJSON_AddStringToObject(ch, "type", ch_type_name(n->ch[c].type));
            cJSON_AddNumberToObject(ch, "max", n->ch[c].max);
            cJSON_AddNumberToObject(ch, "value", n->value[c]);
            if (n->desired_mask & (1u << c)) {
                cJSON_AddNumberToObject(ch, "desired", n->desired[c]);
            }
            cJSON_AddItemToArray(chs, ch);
        }
        cJSON_AddItemToArray(arr, j);
    }
}

cJSON *gw_api_nodes_json(void)
{
    cJSON *o = cJSON_CreateObject();
    NODES_LOCK();
    gw_link_snapshot(s_nodes);
    add_nodes(o);
    NODES_UNLOCK();
    return o;
}

cJSON *gw_api_info_json(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    gw_wifi_status_t ws;
    gw_wifi_get_status(&ws);
    gw_ota_status_t os;
    gw_ota_get_status(&os);
    gw_cloud_status_t cs;
    gw_cloud_get_status(&cs);
    int total = 0, online = 0;
    NODES_LOCK();
    gw_link_snapshot(s_nodes);
    for (int i = 0; i < GW_MAX_NODES; i++) {
        total += s_nodes[i].used;
        online += s_nodes[i].used && s_nodes[i].online;
    }
    NODES_UNLOCK();
    uint8_t mac[6];
    char mac_str[18];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(mac));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", app->project_name);
    cJSON_AddStringToObject(o, "version", app->version);
    cJSON_AddStringToObject(o, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddStringToObject(o, "idf", app->idf_ver);
    cJSON_AddStringToObject(o, "mac", mac_str);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(w, "ssid", ws.ssid);
    cJSON_AddBoolToObject(w, "connected", ws.connected);
    cJSON_AddStringToObject(w, "ip", ws.ip);
    cJSON_AddNumberToObject(w, "rssi", ws.rssi);
    cJSON_AddNumberToObject(w, "channel", ws.channel);
    cJSON *n = cJSON_AddObjectToObject(o, "nodes");
    cJSON_AddNumberToObject(n, "total", total);
    cJSON_AddNumberToObject(n, "online", online);
    cJSON_AddNumberToObject(n, "pair_left", gw_link_pair_remaining());
    cJSON *ota = cJSON_AddObjectToObject(o, "ota");
    cJSON_AddStringToObject(ota, "state", gw_ota_state_name(os.state));
    cJSON_AddNumberToObject(ota, "received", os.received);
    cJSON_AddNumberToObject(ota, "total", os.total);
    if (os.version[0]) {
        cJSON_AddStringToObject(ota, "version", os.version);
    }
    if (os.error[0]) {
        cJSON_AddStringToObject(ota, "error", os.error);
    }
    /* 上次升级的固件没通过开机确认、已回滚：告诉 App 是哪个版本 */
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bad_desc;
    if (bad && esp_ota_get_partition_description(bad, &bad_desc) == ESP_OK) {
        cJSON_AddStringToObject(ota, "rolled_back", bad_desc.version);
    }
    cJSON *c = cJSON_AddObjectToObject(o, "cloud");
    cJSON_AddBoolToObject(c, "configured", cs.configured);
    cJSON_AddBoolToObject(c, "connected", cs.connected);
    cJSON_AddStringToObject(c, "id", gw_cloud_id());
    return o;
}

bool gw_api_token_ok(const char *tok)
{
    const char *token = gw_prov_api_token();
    if (!tok || !token[0] || strlen(tok) != strlen(token)) {
        return false;
    }
    /* 定长比较，不因提前退出泄露匹配位置 */
    uint8_t diff = 0;
    for (size_t i = 0; token[i]; i++) {
        diff |= (uint8_t)(tok[i] ^ token[i]);
    }
    return diff == 0;
}

/* ---------- 命令（HTTP 与云端共用），成功返回 NULL，否则返回错误码 ---------- */

static const char *find_node_idx(const cJSON *body, int *idx)
{
    uint8_t mac[6];
    if (!parse_mac(cJSON_GetStringValue(cJSON_GetObjectItem(body, "mac")), mac)) {
        return "bad_mac";
    }
    for (int i = 0; i < GW_MAX_NODES; i++) {
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) {
            *idx = i;
            return NULL;
        }
    }
    return "no_such_node";
}

static const char *op_set(const cJSON *body)
{
    int idx;
    NODES_LOCK();
    gw_link_snapshot(s_nodes);
    const char *err = find_node_idx(body, &idx);
    if (!err) {
        const cJSON *ch = cJSON_GetObjectItem(body, "ch");
        const cJSON *val = cJSON_GetObjectItem(body, "value");
        const gw_node_t *n = &s_nodes[idx];
        if (!cJSON_IsNumber(ch) || !cJSON_IsNumber(val) || ch->valueint < 0 || ch->valueint >= n->n_ch ||
            val->valuedouble < 0 || val->valuedouble > n->ch[ch->valueint].max) {
            err = "bad_channel_or_value";
        } else {
            esp_err_t e = gw_link_set(idx, (uint8_t)ch->valueint, (uint16_t)val->valueint);
            err = e == ESP_OK ? NULL : e == ESP_ERR_INVALID_STATE ? "offline" : "bad_channel_or_value";
        }
    }
    NODES_UNLOCK();
    return err;
}

static const char *op_unpair(const cJSON *body)
{
    int idx;
    NODES_LOCK();
    gw_link_snapshot(s_nodes);
    const char *err = find_node_idx(body, &idx);
    NODES_UNLOCK();
    if (!err && gw_link_unpair(idx) != ESP_OK) {
        err = "unpair_failed";
    }
    return err;
}

static const char *op_pair(const cJSON *body)
{
    const cJSON *sec = cJSON_GetObjectItem(body, "seconds");
    int s = cJSON_IsNumber(sec) ? sec->valueint : -1;
    if (s < 0 || s > 300) {
        return "bad_seconds";
    }
    gw_link_pair((uint32_t)s);
    return NULL;
}

const char *gw_api_exec(const cJSON *cmd)
{
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "op"));
    if (!op) {
        return "bad_op";
    }
    if (strcmp(op, "set") == 0) {
        return op_set(cmd);
    } else if (strcmp(op, "unpair") == 0) {
        return op_unpair(cmd);
    } else if (strcmp(op, "pair") == 0) {
        return op_pair(cmd);
    } else if (strcmp(op, "get") == 0) {
        return NULL;
    } else if (strcmp(op, "ota") == 0) {
        /* 远程升级：网关自己下载，结果看 state 里的 ota */
        const cJSON *force = cJSON_GetObjectItem(cmd, "force");
        return gw_ota_start_url(cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "url")),
                                cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "sha256")),
                                cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "version")),
                                cJSON_IsTrue(force));
    }
    return "bad_op";
}

static const char *http_status(const char *err)
{
    if (strcmp(err, "no_such_node") == 0) {
        return "404 Not Found";
    } else if (strcmp(err, "offline") == 0) {
        return "409 Conflict";
    } else if (strcmp(err, "unpair_failed") == 0) {
        return "500 Internal Server Error";
    }
    return "400 Bad Request";
}

/* ---------- 节点 ---------- */

static esp_err_t nodes_get(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    return send_json(req, gw_api_nodes_json());
}

/* POST 命令：读 JSON 请求体，执行 op */
static esp_err_t op_post(httpd_req_t *req, const char *(*op)(const cJSON *))
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = recv_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *err = op(body);
    cJSON_Delete(body);
    if (err) {
        return gw_http_error(req, http_status(err), err);
    }
    gw_cloud_notify();
    return send_ok(req);
}

static esp_err_t set_post(httpd_req_t *req)
{
    return op_post(req, op_set);
}

static esp_err_t unpair_post(httpd_req_t *req)
{
    return op_post(req, op_unpair);
}

static esp_err_t pair_post(httpd_req_t *req)
{
    return op_post(req, op_pair);
}

/* ---------- 云端配置 ---------- */

static esp_err_t cloud_get(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    gw_cloud_status_t cs;
    gw_cloud_get_status(&cs);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "configured", cs.configured);
    cJSON_AddBoolToObject(o, "connected", cs.connected);
    cJSON_AddStringToObject(o, "id", gw_cloud_id());
    cJSON_AddStringToObject(o, "host", cs.host);
    cJSON_AddNumberToObject(o, "port", cs.port);
    cJSON_AddStringToObject(o, "username", cs.user);
    /* 已鉴权的 App 本就能控制网关，下发密码让它自动同步云端配置 */
    char pass[65];
    gw_cloud_get_password(pass, sizeof(pass));
    cJSON_AddStringToObject(o, "password", pass);
    cJSON_AddStringToObject(o, "error", cs.error);
    return send_json(req, o);
}

static esp_err_t cloud_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = recv_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(body, "host"));
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(body, "username"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(body, "password"));
    const cJSON *port = cJSON_GetObjectItem(body, "port");
    esp_err_t err = gw_cloud_config(host ? host : "", cJSON_IsNumber(port) ? port->valueint : 8883,
                                    user ? user : "", pass ? pass : "");
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return gw_http_error(req, "400 Bad Request", "bad_config");
    }
    return send_ok(req);
}

/* ---------- 绑定 ---------- */

/* 超时处理，调用方持锁 */
static void bind_expire_locked(int64_t now)
{
    if (s_bind.state == BIND_PENDING && now - s_bind.since_us > BIND_TIMEOUT_US) {
        s_bind.state = BIND_DENIED;
        s_bind.since_us = now;
    } else if ((s_bind.state == BIND_ALLOWED || s_bind.state == BIND_DENIED) &&
               now - s_bind.since_us > BIND_KEEP_US) {
        s_bind.state = BIND_NONE;
    }
}

static esp_err_t bind_post(httpd_req_t *req)
{
    cJSON *body = recv_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(body, "name"));
    char nm[GW_API_NAME_LEN + 1];
    strlcpy(nm, name && name[0] ? name : "未命名", sizeof(nm));
    cJSON_Delete(body);
    /* 去掉控制字符，避免屏幕上显示乱码 */
    for (char *p = nm; *p; p++) {
        if ((unsigned char)*p < 0x20) {
            *p = ' ';
        }
    }

    int64_t now = esp_timer_get_time();
    uint32_t id = esp_random() | 1;
    bool busy;
    taskENTER_CRITICAL(&s_lock);
    bind_expire_locked(now);
    busy = s_bind.state == BIND_PENDING;
    if (!busy) {
        s_bind.state = BIND_PENDING;
        s_bind.id = id;
        s_bind.since_us = now;
        strlcpy(s_bind.name, nm, sizeof(s_bind.name));
    }
    taskEXIT_CRITICAL(&s_lock);
    if (busy) {
        return gw_http_error(req, "409 Conflict", "busy");
    }
    ESP_LOGI(TAG, "bind request from \"%s\"", nm);
    if (s_bind_cb) {
        s_bind_cb();
    }
    cJSON *o = cJSON_CreateObject();
    char ids[12];
    snprintf(ids, sizeof(ids), "%08" PRIx32, id);
    cJSON_AddStringToObject(o, "id", ids);
    cJSON_AddNumberToObject(o, "timeout_s", BIND_TIMEOUT_US / 1000000);
    return send_json(req, o);
}

static esp_err_t bind_get(httpd_req_t *req)
{
    char q[48], ids[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "id", ids, sizeof(ids)) != ESP_OK) {
        return gw_http_error(req, "400 Bad Request", "bad_id");
    }
    uint32_t id = strtoul(ids, NULL, 16);
    bind_state_t st;
    taskENTER_CRITICAL(&s_lock);
    bind_expire_locked(esp_timer_get_time());
    st = (id && id == s_bind.id) ? s_bind.state : BIND_NONE;
    if (st == BIND_ALLOWED) {
        s_bind.state = BIND_NONE;   /* token 只下发一次 */
        s_bind.id = 0;
    }
    taskEXIT_CRITICAL(&s_lock);

    cJSON *o = cJSON_CreateObject();
    switch (st) {
    case BIND_PENDING:
        cJSON_AddStringToObject(o, "state", "pending");
        break;
    case BIND_ALLOWED:
        cJSON_AddStringToObject(o, "state", "allowed");
        cJSON_AddStringToObject(o, "token", gw_prov_api_token());
        ESP_LOGI(TAG, "bind allowed, token delivered");
        break;
    case BIND_DENIED:
        cJSON_AddStringToObject(o, "state", "denied");
        break;
    default:
        cJSON_AddStringToObject(o, "state", "unknown");
        break;
    }
    return send_json(req, o);
}

void gw_api_set_bind_cb(gw_api_bind_cb_t cb)
{
    s_bind_cb = cb;
}

bool gw_api_bind_pending(char name[GW_API_NAME_LEN + 1])
{
    bool pending;
    taskENTER_CRITICAL(&s_lock);
    bind_expire_locked(esp_timer_get_time());
    pending = s_bind.state == BIND_PENDING;
    if (pending) {
        memcpy(name, s_bind.name, GW_API_NAME_LEN + 1);
    }
    taskEXIT_CRITICAL(&s_lock);
    return pending;
}

void gw_api_bind_decide(bool allow)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_bind.state == BIND_PENDING) {
        s_bind.state = allow ? BIND_ALLOWED : BIND_DENIED;
        s_bind.since_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "bind %s on screen", allow ? "allowed" : "denied");
}

esp_err_t gw_api_register(httpd_handle_t httpd)
{
    s_nodes_mtx = xSemaphoreCreateMutex();
    const httpd_uri_t uris[] = {
        {.uri = "/api/cloud", .method = HTTP_GET, .handler = cloud_get},
        {.uri = "/api/cloud", .method = HTTP_POST, .handler = cloud_post},
        {.uri = "/api/nodes", .method = HTTP_GET, .handler = nodes_get},
        {.uri = "/api/nodes/set", .method = HTTP_POST, .handler = set_post},
        {.uri = "/api/nodes/unpair", .method = HTTP_POST, .handler = unpair_post},
        {.uri = "/api/pair", .method = HTTP_POST, .handler = pair_post},
        {.uri = "/api/bind", .method = HTTP_POST, .handler = bind_post},
        {.uri = "/api/bind", .method = HTTP_GET, .handler = bind_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(httpd, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

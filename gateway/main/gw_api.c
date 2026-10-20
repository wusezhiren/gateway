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
#include "gw_api.h"
#include "gw_http.h"
#include "gw_link.h"
#include "gw_prov.h"

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

/* 按 MAC 找节点下标；找不到时已回复 404，返回 -1 */
static int find_node(httpd_req_t *req, cJSON *body, gw_node_t *nodes)
{
    uint8_t mac[6];
    const cJSON *m = cJSON_GetObjectItem(body, "mac");
    if (!parse_mac(cJSON_GetStringValue(m), mac)) {
        gw_http_error(req, "400 Bad Request", "bad_mac");
        return -1;
    }
    gw_link_snapshot(nodes);
    for (int i = 0; i < GW_MAX_NODES; i++) {
        if (nodes[i].used && memcmp(nodes[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    gw_http_error(req, "404 Not Found", "no_such_node");
    return -1;
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

/* 节点表放静态区，避免占 httpd 栈；httpd 单任务，处理函数不会并发 */
static gw_node_t s_nodes[GW_MAX_NODES];

/* ---------- 节点 ---------- */

static esp_err_t nodes_get(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    gw_link_snapshot(s_nodes);
    int64_t now = esp_timer_get_time();
    cJSON *o = cJSON_CreateObject();
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
    return send_json(req, o);
}

static esp_err_t set_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = recv_json(req);
    if (!body) {
        return ESP_OK;
    }
    int idx = find_node(req, body, s_nodes);
    if (idx < 0) {
        cJSON_Delete(body);
        return ESP_OK;
    }
    const cJSON *ch = cJSON_GetObjectItem(body, "ch");
    const cJSON *val = cJSON_GetObjectItem(body, "value");
    const gw_node_t *n = &s_nodes[idx];
    if (!cJSON_IsNumber(ch) || !cJSON_IsNumber(val) || ch->valueint < 0 || ch->valueint >= n->n_ch ||
        val->valuedouble < 0 || val->valuedouble > n->ch[ch->valueint].max) {
        cJSON_Delete(body);
        return gw_http_error(req, "400 Bad Request", "bad_channel_or_value");
    }
    esp_err_t err = gw_link_set(idx, (uint8_t)ch->valueint, (uint16_t)val->valueint);
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) {
        return gw_http_error(req, "409 Conflict", "offline");
    } else if (err != ESP_OK) {
        return gw_http_error(req, "400 Bad Request", "bad_channel_or_value");
    }
    return send_ok(req);
}

static esp_err_t unpair_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = recv_json(req);
    if (!body) {
        return ESP_OK;
    }
    int idx = find_node(req, body, s_nodes);
    cJSON_Delete(body);
    if (idx < 0) {
        return ESP_OK;
    }
    if (gw_link_unpair(idx) != ESP_OK) {
        return gw_http_error(req, "500 Internal Server Error", "unpair_failed");
    }
    return send_ok(req);
}

static esp_err_t pair_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    cJSON *body = recv_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *sec = cJSON_GetObjectItem(body, "seconds");
    int s = cJSON_IsNumber(sec) ? sec->valueint : -1;
    cJSON_Delete(body);
    if (s < 0 || s > 300) {
        return gw_http_error(req, "400 Bad Request", "bad_seconds");
    }
    gw_link_pair((uint32_t)s);
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
    const httpd_uri_t uris[] = {
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

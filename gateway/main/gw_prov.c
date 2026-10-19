#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "qrcode.h"
#include "gw_prov.h"

static const char *TAG = "gw_prov";

#define IDLE_TIMEOUT_US     (10 * 60 * 1000000LL)
#define DONE_CLOSE_US       (30 * 1000000LL)
#define API_NVS_NS          "gw_api"
#define MAX_BODY            256
#define SCAN_MAX            20
#define CONNECT_DELAY_MS    500     /* 先让 /connect 的响应发出去，再开始连路由器（会切信道） */

extern const char prov_html_start[] asm("_binary_prov_html_start");
extern const char prov_html_end[] asm("_binary_prov_html_end");

static SemaphoreHandle_t s_lock;
static gw_prov_status_t s_st;
static int64_t s_deadline_us;
static gw_prov_cb_t s_cb;
static TaskHandle_t s_task;
static httpd_handle_t s_httpd;
static volatile bool s_dns_run;
static volatile TaskHandle_t s_dns_task;
static char s_ap_ip[16] = "192.168.4.1";
static char s_portal_uri[32];
static char s_token[33];

/* /connect 收到的待试连凭据，由配网任务发起 */
static bool s_connect_pending;
static char s_pending_pass[65];

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

const char *gw_prov_state_name(gw_prov_state_t s)
{
    static const char *const names[] = {"off", "waiting", "client", "connecting", "done", "failed"};
    return s <= GW_PROV_FAILED ? names[s] : "?";
}

const char *gw_try_state_name(gw_try_state_t s)
{
    static const char *const names[] = {"idle", "connecting", "ok", "bad_password", "no_ap", "no_ip", "failed"};
    return s <= GW_TRY_FAILED ? names[s] : "?";
}

static void notify(void)
{
    if (s_cb) {
        gw_prov_status_t st;
        gw_prov_get_status(&st);
        s_cb(&st);
    }
}

static void touch(void)
{
    LOCK();
    if (s_st.state != GW_PROV_OFF && s_st.state != GW_PROV_DONE) {
        s_deadline_us = esp_timer_get_time() + IDLE_TIMEOUT_US;
    }
    UNLOCK();
}

const char *gw_prov_api_token(void)
{
    if (s_token[0]) {
        return s_token;
    }
    nvs_handle_t h;
    size_t len = sizeof(s_token);
    if (nvs_open(API_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return "";
    }
    if (nvs_get_str(h, "token", s_token, &len) != ESP_OK || strlen(s_token) != 32) {
        uint8_t rnd[16];
        esp_fill_random(rnd, sizeof(rnd));
        for (int i = 0; i < 16; i++) {
            sprintf(&s_token[i * 2], "%02x", rnd[i]);
        }
        nvs_set_str(h, "token", s_token);
        nvs_commit(h);
        ESP_LOGI(TAG, "api token generated");
    }
    nvs_close(h);
    return s_token;
}

/* ---------- DNS：所有 A 记录都解析到热点 IP ---------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval tv = {.tv_sec = 1};
    uint32_t ip = inet_addr(s_ap_ip);

    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns bind failed");
        goto out;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (s_dns_run) {
        uint8_t buf[512];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12 || (buf[2] & 0x80) || ((buf[4] << 8) | buf[5]) < 1) {
            continue;   /* 太短、是应答、或没有问题段 */
        }
        /* 跳过第一个问题的 QNAME */
        int p = 12;
        while (p < n && buf[p] != 0) {
            if (buf[p] & 0xC0) {
                p = n;
                break;
            }
            p += buf[p] + 1;
        }
        if (p + 5 > n) {
            continue;
        }
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int len = p + 5;    /* 问题段结束，丢弃其后的附加记录（如 EDNS） */

        buf[2] = 0x80 | (buf[2] & 0x79) | 0x04;   /* QR=1，保留 opcode/RD，AA=1 */
        buf[3] = 0x80;                              /* RA=1，RCODE=0 */
        buf[4] = 0; buf[5] = 1;                     /* QDCOUNT=1 */
        buf[6] = 0; buf[7] = 0;
        memset(&buf[8], 0, 4);                      /* NSCOUNT=ARCOUNT=0 */
        if (qtype == 1 && len + 16 <= (int)sizeof(buf)) {
            static const uint8_t ans_hdr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(&buf[len], ans_hdr, sizeof(ans_hdr));
            memcpy(&buf[len + sizeof(ans_hdr)], &ip, 4);
            len += sizeof(ans_hdr) + 4;
            buf[7] = 1;                             /* ANCOUNT=1 */
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, from_len);
    }
out:
    if (sock >= 0) {
        close(sock);
    }
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    s_dns_run = true;
    TaskHandle_t t;
    if (xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, &t) == pdPASS) {
        s_dns_task = t;
    }
}

static void dns_stop(void)
{
    s_dns_run = false;
    for (int i = 0; i < 40 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- HTTP ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

static esp_err_t root_get(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, prov_html_start, prov_html_end - prov_html_start);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    touch();
    static wifi_ap_record_t recs[SCAN_MAX];
    int n = gw_wifi_scan(recs, SCAN_MAX);
    if (n < 0) {
        return send_error(req, "503 Service Unavailable", "busy");
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    touch();
    char body[MAX_BODY + 1];
    if (req->content_len <= 0 || req->content_len > MAX_BODY) {
        return send_error(req, "400 Bad Request", "bad_body");
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    cJSON *o = cJSON_Parse(body);
    const cJSON *jssid = cJSON_GetObjectItem(o, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(o, "password");
    const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : NULL;
    const char *pass = cJSON_IsString(jpass) ? jpass->valuestring : "";
    size_t slen = ssid ? strlen(ssid) : 0;
    size_t plen = strlen(pass);
    if (slen < 1 || slen > 32 || (plen != 0 && (plen < 8 || plen > 63))) {
        cJSON_Delete(o);
        return send_error(req, "400 Bad Request", "invalid_ssid_or_password");
    }

    LOCK();
    if (s_st.state == GW_PROV_OFF || s_st.state == GW_PROV_DONE || s_st.state == GW_PROV_CONNECTING) {
        UNLOCK();
        cJSON_Delete(o);
        return send_error(req, "409 Conflict", "busy");
    }
    s_st.state = GW_PROV_CONNECTING;
    strlcpy(s_st.target, ssid, sizeof(s_st.target));
    strlcpy(s_pending_pass, pass, sizeof(s_pending_pass));
    s_st.result = GW_TRY_CONNECTING;
    s_connect_pending = true;
    UNLOCK();
    cJSON_Delete(o);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    esp_err_t err = send_json(req, r);
    notify();
    xTaskNotifyGive(s_task);
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    gw_prov_status_t st;
    gw_prov_get_status(&st);
    if (st.state == GW_PROV_CONNECTING || st.state == GW_PROV_DONE) {
        /* 用于确认切信道期间手机是否还能访问网关 */
        ESP_LOGD(TAG, "status poll: %s", gw_prov_state_name(st.state));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", gw_prov_state_name(st.state));
    cJSON_AddStringToObject(o, "ssid", st.target);
    if (st.state == GW_PROV_FAILED) {
        cJSON_AddStringToObject(o, "reason", gw_try_state_name(st.result));
    }
    if (st.state == GW_PROV_DONE) {
        cJSON_AddStringToObject(o, "ip", st.ip);
        cJSON_AddStringToObject(o, "host", "esp-gw.local");
        cJSON_AddStringToObject(o, "token", gw_prov_api_token());
    }
    cJSON_AddNumberToObject(o, "close_in", st.remaining_s);
    return send_json(req, o);
}

/* 系统联网检测（generate_204、hotspot-detect.html 等）都会落到这里，重定向触发配网页弹出 */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_uri);
    /* iOS 需要响应体才能识别强制门户 */
    return httpd_resp_sendstr(req, "Redirect to setup page");
}

static esp_err_t httpd_up(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/prov/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/prov/connect", .method = HTTP_POST, .handler = connect_post},
        {.uri = "/api/prov/status", .method = HTTP_GET, .handler = status_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    return ESP_OK;
}

/* ---------- 状态机 ---------- */

static void set_captive_dhcp_option(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (!ap || esp_netif_get_ip_info(ap, &info) != ESP_OK) {
        return;
    }
    snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&info.ip));
    snprintf(s_portal_uri, sizeof(s_portal_uri), "http://%s/", s_ap_ip);
    /* DHCP option 114：较新的 Android/iOS 直接据此弹出配网页 */
    esp_netif_dhcps_stop(ap);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri, strlen(s_portal_uri));
    esp_netif_dhcps_start(ap);
}

static void print_join_qr(void)
{
    char text[128];
    snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", s_st.ap_ssid, s_st.ap_pass);
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, text);
}

esp_err_t gw_prov_start(void)
{
    LOCK();
    if (s_st.state != GW_PROV_OFF) {
        UNLOCK();
        return ESP_OK;
    }
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    memset(&s_st, 0, sizeof(s_st));
    snprintf(s_st.ap_ssid, sizeof(s_st.ap_ssid), "ESP-GW-%02X%02X", mac[4], mac[5]);
    size_t fixed_len = strlen(CONFIG_GW_PROV_AP_PASSWORD);
    if (fixed_len >= 8 && fixed_len <= 63) {
        strlcpy(s_st.ap_pass, CONFIG_GW_PROV_AP_PASSWORD, sizeof(s_st.ap_pass));
    } else {
        if (fixed_len) {
            ESP_LOGW(TAG, "GW_PROV_AP_PASSWORD must be 8-63 chars, using random password");
        }
        static const char charset[] = "abcdefghjkmnpqrstuvwxyz23456789";    /* 去掉易混字符 */
        for (int i = 0; i < 8; i++) {
            s_st.ap_pass[i] = charset[esp_random() % (sizeof(charset) - 1)];
        }
        s_st.ap_pass[8] = '\0';
    }

    esp_err_t err = gw_wifi_prov_begin(s_st.ap_ssid, s_st.ap_pass);
    if (err != ESP_OK) {
        UNLOCK();
        return err;
    }
    set_captive_dhcp_option();
    dns_start();
    err = httpd_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        dns_stop();
        gw_wifi_prov_end();
        UNLOCK();
        return err;
    }
    s_st.state = GW_PROV_WAITING;
    s_deadline_us = esp_timer_get_time() + IDLE_TIMEOUT_US;
    UNLOCK();

    ESP_LOGI(TAG, "provisioning started: ssid \"%s\" password \"%s\", open http://%s/",
             s_st.ap_ssid, s_st.ap_pass, s_ap_ip);
    print_join_qr();
    notify();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void gw_prov_stop(void)
{
    LOCK();
    if (s_st.state == GW_PROV_OFF) {
        UNLOCK();
        return;
    }
    s_st.state = GW_PROV_OFF;
    s_connect_pending = false;
    memset(s_pending_pass, 0, sizeof(s_pending_pass));
    httpd_handle_t httpd = s_httpd;
    s_httpd = NULL;
    UNLOCK();

    /* 在锁外停服务：httpd_stop 要等处理函数返回，而处理函数可能正在等锁 */
    if (httpd) {
        httpd_stop(httpd);
    }
    dns_stop();
    gw_wifi_prov_end();
    ESP_LOGI(TAG, "provisioning stopped");
    notify();
}

void gw_prov_get_status(gw_prov_status_t *st)
{
    LOCK();
    *st = s_st;
    int64_t left = s_deadline_us - esp_timer_get_time();
    st->remaining_s = (st->state != GW_PROV_OFF && left > 0) ? (uint32_t)(left / 1000000) : 0;
    UNLOCK();
}

gw_prov_key_result_t gw_prov_on_long_press(void)
{
    if (s_st.state != GW_PROV_OFF) {
        gw_prov_stop();
        return GW_PROV_KEY_STOPPED;
    }
    gw_wifi_status_t ws;
    gw_wifi_get_status(&ws);
    if (ws.connected) {
        ESP_LOGW(TAG, "already online, confirm on screen (or 'prov start') to re-provision");
        return GW_PROV_KEY_NEED_CONFIRM;
    }
    gw_prov_start();
    return GW_PROV_KEY_STARTED;
}

/* 每秒检查：试连结果、超时关闭 */
static void prov_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        bool changed = false, stop = false;

        if (s_connect_pending) {
            vTaskDelay(pdMS_TO_TICKS(CONNECT_DELAY_MS));
            char ssid[33], pass[65];
            LOCK();
            s_connect_pending = false;
            strlcpy(ssid, s_st.target, sizeof(ssid));
            strlcpy(pass, s_pending_pass, sizeof(pass));
            memset(s_pending_pass, 0, sizeof(s_pending_pass));
            bool still = s_st.state == GW_PROV_CONNECTING;
            UNLOCK();
            /* 同步把试连状态置为 CONNECTING，下面的检查不会读到上一次的结果 */
            if (still && gw_wifi_try_connect(ssid, pass) != ESP_OK) {
                LOCK();
                s_st.state = GW_PROV_FAILED;
                s_st.result = GW_TRY_FAILED;
                UNLOCK();
                notify();
            }
        }

        LOCK();
        int64_t now = esp_timer_get_time();
        if (s_st.state == GW_PROV_CONNECTING) {
            gw_try_state_t t = gw_wifi_try_state();
            if (t == GW_TRY_OK) {
                gw_wifi_status_t ws;
                gw_wifi_get_status(&ws);
                s_st.state = GW_PROV_DONE;
                s_st.result = t;
                strlcpy(s_st.ip, ws.ip, sizeof(s_st.ip));
                s_deadline_us = now + DONE_CLOSE_US;
                gw_prov_api_token();
                ESP_LOGI(TAG, "connected to \"%s\", ip %s, softap closes in 30 s", s_st.target, s_st.ip);
                changed = true;
            } else if (t != GW_TRY_CONNECTING) {
                s_st.state = GW_PROV_FAILED;
                s_st.result = t;
                s_deadline_us = now + IDLE_TIMEOUT_US;
                ESP_LOGW(TAG, "connect \"%s\" failed: %s", s_st.target, gw_try_state_name(t));
                changed = true;
            }
        }
        if (s_st.state != GW_PROV_OFF && now >= s_deadline_us) {
            ESP_LOGI(TAG, "%s", s_st.state == GW_PROV_DONE ? "closing softap" : "provisioning idle timeout");
            stop = true;
        }
        UNLOCK();

        if (stop) {
            gw_prov_stop();
        } else if (changed) {
            notify();
        }
    }
}

static void ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    LOCK();
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_st.clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_st.clients > 0) {
        s_st.clients--;
    }
    bool changed = false;
    if (s_st.state == GW_PROV_WAITING && s_st.clients > 0) {
        s_st.state = GW_PROV_CLIENT;
        changed = true;
    } else if (s_st.state == GW_PROV_CLIENT && s_st.clients == 0) {
        s_st.state = GW_PROV_WAITING;
        changed = true;
    }
    UNLOCK();
    ESP_LOGI(TAG, "phone %s softap (%d connected)",
             id == WIFI_EVENT_AP_STACONNECTED ? "joined" : "left", s_st.clients);
    if (changed) {
        notify();
    }
}

esp_err_t gw_prov_init(gw_prov_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateRecursiveMutex();
    snprintf(s_portal_uri, sizeof(s_portal_uri), "http://%s/", s_ap_ip);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event, NULL));
    /* 关掉重定向产生的大量 httpd 警告 */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    if (xTaskCreate(prov_task, "gw_prov", 4096, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "gw_http.h"
#include "gw_link.h"
#include "gw_ota.h"
#include "gw_prov.h"
#include "gw_wifi.h"

static const char *TAG = "gw_http";

static httpd_handle_t s_httpd;

esp_err_t gw_http_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, body);
}

bool gw_http_authorized(httpd_req_t *req)
{
    char hdr[48];
    const char *token = gw_prov_api_token();
    if (token[0] && httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Bearer ", 7) == 0 && strlen(hdr + 7) == strlen(token)) {
        /* 定长比较，不因提前退出泄露匹配长度 */
        uint8_t diff = 0;
        for (size_t i = 0; token[i]; i++) {
            diff |= (uint8_t)(hdr[7 + i] ^ token[i]);
        }
        if (diff == 0) {
            return true;
        }
    }
    gw_http_error(req, "401 Unauthorized", "unauthorized");
    return false;
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    gw_wifi_status_t ws;
    gw_wifi_get_status(&ws);
    gw_ota_status_t os;
    gw_ota_get_status(&os);
    static gw_node_t nodes[GW_MAX_NODES];
    gw_link_snapshot(nodes);
    int total = 0, online = 0;
    for (int i = 0; i < GW_MAX_NODES; i++) {
        total += nodes[i].used;
        online += nodes[i].used && nodes[i].online;
    }
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

esp_err_t gw_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t info = {.uri = "/api/info", .method = HTTP_GET, .handler = info_get};
    httpd_register_uri_handler(s_httpd, &info);
    /* 关掉配网重定向时产生的大量 httpd 警告 */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    return gw_ota_register(s_httpd);
}

httpd_handle_t gw_http_handle(void)
{
    return s_httpd;
}

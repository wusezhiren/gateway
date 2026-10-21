#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "gw_api.h"
#include "gw_http.h"
#include "gw_ota.h"
#include "gw_node_ota.h"

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
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Bearer ", 7) == 0 && gw_api_token_ok(hdr + 7)) {
        return true;
    }
    gw_http_error(req, "401 Unauthorized", "unauthorized");
    return false;
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *o = gw_api_info_json();
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
    cfg.max_uri_handlers = 24;
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
    err = gw_ota_register(s_httpd);
    if (err == ESP_OK) {
        err = gw_node_ota_register(s_httpd);
    }
    if (err == ESP_OK) {
        err = gw_api_register(s_httpd);
    }
    return err;
}

httpd_handle_t gw_http_handle(void)
{
    return s_httpd;
}

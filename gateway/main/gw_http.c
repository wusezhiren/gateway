#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "gw_api.h"
#include "gw_http.h"
#include "gw_ota.h"
#include "gw_node_ota.h"
#include "gw_audio.h"
#include "gw_sd.h"
#include "gw_wake.h"

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

static esp_err_t audio_play_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) return ESP_FAIL;
    if (req->content_len <= 0 || req->content_len > 256 * 1024) return gw_http_error(req,"400 Bad Request","invalid_audio_size");
    uint8_t *buf=malloc(req->content_len); if(!buf) return gw_http_error(req,"503 Service Unavailable","no_mem");
    int got=0; while(got<req->content_len){int n=httpd_req_recv(req,(char*)buf+got,req->content_len-got);if(n<=0){free(buf);return gw_http_error(req,"400 Bad Request","receive_failed");}got+=n;}
    esp_err_t err=gw_audio_play_wav(buf,got); free(buf); if(err!=ESP_OK)return gw_http_error(req,"400 Bad Request",esp_err_to_name(err)); return httpd_resp_sendstr(req,"{\"ok\":true}");
}

static esp_err_t sd_model_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) return ESP_FAIL;
    if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) return gw_http_error(req,"400 Bad Request","invalid_model_size");
    char name[32]; if (httpd_req_get_url_query_str(req,name,sizeof(name))!=ESP_OK) return gw_http_error(req,"400 Bad Request","filename_required");
    char filename[32]; if(httpd_query_key_value(name,"name",filename,sizeof(filename))!=ESP_OK) return gw_http_error(req,"400 Bad Request","filename_required");
    uint8_t *buf=malloc(req->content_len);if(!buf)return gw_http_error(req,"503 Service Unavailable","no_mem");
    int got=0;while(got<req->content_len){int n=httpd_req_recv(req,(char*)buf+got,req->content_len-got);if(n<=0){free(buf);return gw_http_error(req,"400 Bad Request","receive_failed");}got+=n;}
    esp_err_t err=gw_sd_upload_file(filename,buf,got);free(buf);
    if(err!=ESP_OK)return gw_http_error(req,"400 Bad Request","model_write_failed");
    return httpd_resp_sendstr(req,"{\"ok\":true}");
}

static esp_err_t wake_test_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) return ESP_FAIL;
    if (req->content_len <= 0 || req->content_len > 320000 || (req->content_len & 1))
        return gw_http_error(req, "400 Bad Request", "invalid_pcm_size");
    uint8_t *buf = malloc(req->content_len);
    if (!buf) return gw_http_error(req, "503 Service Unavailable", "no_mem");
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, (char *)buf + got, req->content_len - got);
        if (n <= 0) { free(buf); return gw_http_error(req, "400 Bad Request", "receive_failed"); }
        got += n;
    }
    esp_err_t err = gw_wake_test_pcm(buf, got);
    free(buf);
    if (err != ESP_OK) return gw_http_error(req, "503 Service Unavailable", "wake_test_failed");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t audio_record_get(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) return ESP_FAIL;
    int slot = 0;
    char query[32], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "slot", value, sizeof(value)) == ESP_OK) {
        if (!strcmp(value, "2")) slot = 2;
        else if (strcmp(value, "0")) return gw_http_error(req, "400 Bad Request", "invalid_slot");
    }
    const size_t samples = 16000 * 4;
    int16_t *pcm = malloc(samples * sizeof(int16_t));
    if (!pcm) return gw_http_error(req, "503 Service Unavailable", "no_mem");
    esp_err_t err = gw_audio_record_pcm(pcm, samples, slot);
    if (err != ESP_OK) { free(pcm); return gw_http_error(req, "503 Service Unavailable", "record_failed"); }
    httpd_resp_set_type(req, "application/octet-stream");
    err = httpd_resp_send(req, (const char *)pcm, samples * sizeof(int16_t));
    free(pcm);
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
    const httpd_uri_t audio = {.uri="/api/audio/play",.method=HTTP_POST,.handler=audio_play_post}; httpd_register_uri_handler(s_httpd,&audio);
    const httpd_uri_t model = {.uri="/api/sd/model",.method=HTTP_POST,.handler=sd_model_post}; httpd_register_uri_handler(s_httpd,&model);
    const httpd_uri_t wake_test = {.uri="/api/wake/test",.method=HTTP_POST,.handler=wake_test_post}; httpd_register_uri_handler(s_httpd,&wake_test);
    const httpd_uri_t audio_record = {.uri="/api/audio/record",.method=HTTP_GET,.handler=audio_record_get}; httpd_register_uri_handler(s_httpd,&audio_record);
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

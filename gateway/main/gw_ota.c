#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_http.h"
#include "gw_ota.h"

static const char *TAG = "gw_ota";

#define CHUNK           4096
#define RECV_RETRIES    5
#define REBOOT_DELAY_MS 1500
/* 镜像头 + 第一个段头之后就是 esp_app_desc_t */
#define DESC_OFFSET     (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEAD_NEED       (DESC_OFFSET + sizeof(esp_app_desc_t))

static SemaphoreHandle_t s_lock;
static gw_ota_status_t s_st;
static gw_ota_cb_t s_cb;

const char *gw_ota_state_name(gw_ota_state_t s)
{
    static const char *const names[] = {"idle", "receiving", "rebooting", "failed"};
    return s <= GW_OTA_FAILED ? names[s] : "?";
}

int gw_ota_version_cmp(const char *a, const char *b)
{
    int va[3] = {0}, vb[3] = {0};
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] < vb[i] ? -1 : 1;
        }
    }
    return 0;
}

static void notify(void)
{
    if (s_cb) {
        gw_ota_status_t st;
        gw_ota_get_status(&st);
        s_cb(&st);
    }
}

static void set_state(gw_ota_state_t state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = state;
    strlcpy(s_st.error, error ? error : "", sizeof(s_st.error));
    xSemaphoreGive(s_lock);
    notify();
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    ESP_LOGW(TAG, "rebooting into new firmware");
    esp_restart();
}

static bool query_force(httpd_req_t *req)
{
    char q[32], v[8];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, "force", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

/* 读满 len 字节（或读到请求结束），返回实际字节数，出错返回 -1 */
static int recv_full(httpd_req_t *req, char *buf, int len)
{
    int got = 0, retries = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= RECV_RETRIES) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        retries = 0;
        got += r;
    }
    return got;
}

/* 失败时记录原因并回复；返回 ESP_OK 让 httpd 继续（丢弃剩余请求体） */
static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *reason)
{
    if (h) {
        esp_ota_abort(h);
    }
    ESP_LOGW(TAG, "ota failed: %s", reason);
    set_state(GW_OTA_FAILED, reason);
    return gw_http_error(req, status, reason);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_st.state == GW_OTA_RECEIVING || s_st.state == GW_OTA_REBOOTING;
    if (!busy) {
        memset(&s_st, 0, sizeof(s_st));
        s_st.state = GW_OTA_RECEIVING;
        s_st.total = req->content_len;
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        return gw_http_error(req, "409 Conflict", "busy");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *cur = esp_app_get_description();
    bool force = query_force(req);
    int total = req->content_len;
    if (!part) {
        return fail(req, 0, "500 Internal Server Error", "no_ota_partition");
    }
    if (total < (int)HEAD_NEED) {
        return fail(req, 0, "400 Bad Request", "too_small");
    }
    if (total > (int)part->size) {
        return fail(req, 0, "413 Payload Too Large", "too_large");
    }

    char *buf = malloc(CHUNK);
    if (!buf) {
        return fail(req, 0, "500 Internal Server Error", "no_mem");
    }

    /* 先收镜像头，检查工程名和版本，不合格就不擦写 flash */
    int n = recv_full(req, buf, CHUNK < total ? CHUNK : total);
    if (n < (int)HEAD_NEED) {
        free(buf);
        return fail(req, 0, "400 Bad Request", "recv_failed");
    }
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + DESC_OFFSET);
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        free(buf);
        return fail(req, 0, "400 Bad Request", "not_an_app_image");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.version, desc->version, sizeof(s_st.version));
    xSemaphoreGive(s_lock);
    if (strncmp(desc->project_name, cur->project_name, sizeof(desc->project_name)) != 0) {
        free(buf);
        return fail(req, 0, "400 Bad Request", "wrong_project");
    }
    if (!force && gw_ota_version_cmp(desc->version, cur->version) <= 0) {
        free(buf);
        return fail(req, 0, "409 Conflict", "not_newer");
    }
    ESP_LOGI(TAG, "receiving %s v%s (%d bytes) -> %s%s", desc->project_name, desc->version, total,
             part->label, force ? " (forced)" : "");

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        free(buf);
        return fail(req, 0, "500 Internal Server Error", "ota_begin_failed");
    }

    int received = 0, last_pct = 0;
    while (n > 0) {
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            free(buf);
            return fail(req, h, "500 Internal Server Error", "flash_write_failed");
        }
        received += n;
        int pct = (int)((int64_t)received * 100 / total);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.received = received;
        xSemaphoreGive(s_lock);
        if (pct >= last_pct + 5) {
            last_pct = pct;
            notify();
        }
        int remain = total - received;
        if (remain <= 0) {
            break;
        }
        n = recv_full(req, buf, remain < CHUNK ? remain : CHUNK);
        if (n < 0) {
            free(buf);
            return fail(req, h, "400 Bad Request", "recv_failed");
        }
    }
    free(buf);

    /* 校验镜像完整性和签名（SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT） */
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        return fail(req, 0, "400 Bad Request",
                    err == ESP_ERR_OTA_VALIDATE_FAILED ? "invalid_image_or_signature" : "ota_end_failed");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return fail(req, 0, "500 Internal Server Error", "set_boot_failed");
    }

    set_state(GW_OTA_REBOOTING, NULL);
    ESP_LOGI(TAG, "image verified, boot partition -> %s", part->label);
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"version\":\"%s\",\"reboot_in_ms\":%d}", s_st.version, REBOOT_DELAY_MS);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t gw_ota_register(httpd_handle_t httpd)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    const httpd_uri_t uri = {.uri = "/api/ota/gateway", .method = HTTP_POST, .handler = ota_post};
    return httpd_register_uri_handler(httpd, &uri);
}

void gw_ota_set_cb(gw_ota_cb_t cb)
{
    s_cb = cb;
}

void gw_ota_get_status(gw_ota_status_t *st)
{
    if (!s_lock) {
        memset(st, 0, sizeof(*st));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *st = s_st;
    xSemaphoreGive(s_lock);
}

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "gw_http.h"
#include "gw_ota.h"

static const char *TAG = "gw_ota";

#define CHUNK           4096
#define RECV_RETRIES    5
#define REBOOT_DELAY_MS 1500
#define PULL_TIMEOUT_MS 20000
#define PULL_MAX_REDIRECTS 5
#define PULL_STACK      8192/* 镜像头 + 第一个段头之后就是 esp_app_desc_t */
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

/* 占用升级通道；已有升级在进行时返回 false */
static bool reserve(uint32_t total, const char *version)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_st.state == GW_OTA_RECEIVING || s_st.state == GW_OTA_REBOOTING;
    if (!busy) {
        memset(&s_st, 0, sizeof(s_st));
        s_st.state = GW_OTA_RECEIVING;
        s_st.total = total;
        strlcpy(s_st.version, version ? version : "", sizeof(s_st.version));
    }
    xSemaphoreGive(s_lock);
    if (!busy) {
        notify();
    }
    return !busy;
}

/* ---------- 写入会话：局域网上传和网关自己下载共用 ---------- */

/*
 * 防变砖：只写空闲 OTA 分区，bootloader / 分区表 / 正在运行的分区都不碰；
 * 镜像头不对（工程名、版本）就不擦写；esp_ota_end 校验完整性和签名，
 * 下载的再核对 SHA-256，全部通过才切换启动分区。任何一步失败当前固件都不受影响。
 */
typedef struct {
    const esp_partition_t *part;
    esp_ota_handle_t h;
    int total;
    int received;
    int last_pct;
    mbedtls_sha256_context sha;
} ota_sess_t;

/* 第一块数据（至少 HEAD_NEED 字节）：检查镜像头并开始写 */
static const char *sess_begin(ota_sess_t *s, const char *buf, int n, int total, bool force)
{
    memset(s, 0, sizeof(*s));
    s->total = total;
    s->part = esp_ota_get_next_update_partition(NULL);
    if (!s->part) {
        return "no_ota_partition";
    }
    if (total < (int)HEAD_NEED || n < (int)HEAD_NEED) {
        return "too_small";
    }
    if (total > (int)s->part->size) {
        return "too_large";
    }
    const esp_app_desc_t *cur = esp_app_get_description();
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + DESC_OFFSET);
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "not_an_app_image";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.version, desc->version, sizeof(s_st.version));
    xSemaphoreGive(s_lock);
    if (strncmp(desc->project_name, cur->project_name, sizeof(desc->project_name)) != 0) {
        return "wrong_project";
    }
    if (!force && gw_ota_version_cmp(desc->version, cur->version) <= 0) {
        return "not_newer";
    }
    ESP_LOGI(TAG, "writing %s v%s (%d bytes) -> %s%s", desc->project_name, desc->version, total,
             s->part->label, force ? " (forced)" : "");
    if (esp_ota_begin(s->part, OTA_WITH_SEQUENTIAL_WRITES, &s->h) != ESP_OK) {
        s->h = 0;
        return "ota_begin_failed";
    }
    mbedtls_sha256_init(&s->sha);
    mbedtls_sha256_starts(&s->sha, 0);
    return NULL;
}

static const char *sess_write(ota_sess_t *s, const char *buf, int n)
{
    if (s->received + n > s->total) {
        return "too_large";
    }
    if (esp_ota_write(s->h, buf, n) != ESP_OK) {
        return "flash_write_failed";
    }
    mbedtls_sha256_update(&s->sha, (const unsigned char *)buf, n);
    s->received += n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.received = s->received;
    xSemaphoreGive(s_lock);
    int pct = (int)((int64_t)s->received * 100 / s->total);
    if (pct >= s->last_pct + 5) {
        s->last_pct = pct;
        notify();
    }
    return NULL;
}

/* 收完：核对 SHA-256（可选）、校验签名、切换启动分区 */
static const char *sess_finish(ota_sess_t *s, const uint8_t *expect_sha)
{
    uint8_t sha[32];
    mbedtls_sha256_finish(&s->sha, sha);
    mbedtls_sha256_free(&s->sha);
    if (s->received != s->total) {
        esp_ota_abort(s->h);
        s->h = 0;
        return "incomplete";
    }
    if (expect_sha && memcmp(sha, expect_sha, sizeof(sha)) != 0) {
        esp_ota_abort(s->h);
        s->h = 0;
        return "sha256_mismatch";
    }
    esp_err_t err = esp_ota_end(s->h);
    s->h = 0;
    if (err != ESP_OK) {
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? "invalid_image_or_signature" : "ota_end_failed";
    }
    if (esp_ota_set_boot_partition(s->part) != ESP_OK) {
        return "set_boot_failed";
    }
    ESP_LOGI(TAG, "image verified, boot partition -> %s", s->part->label);
    return NULL;
}

static void sess_abort(ota_sess_t *s)
{
    if (s->h) {
        esp_ota_abort(s->h);
        mbedtls_sha256_free(&s->sha);
        s->h = 0;
    }
}

static void finish_ok(void)
{
    set_state(GW_OTA_REBOOTING, NULL);
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
}

static void finish_fail(const char *reason)
{
    ESP_LOGW(TAG, "ota failed: %s", reason);
    set_state(GW_OTA_FAILED, reason);
}

/* ---------- 局域网上传：POST /api/ota/gateway ---------- */

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

static const char *http_status_of(const char *reason)
{
    if (strcmp(reason, "too_large") == 0) {
        return "413 Payload Too Large";
    } else if (strcmp(reason, "not_newer") == 0 || strcmp(reason, "busy") == 0) {
        return "409 Conflict";
    } else if (strstr(reason, "failed") && strcmp(reason, "recv_failed") != 0) {
        return "500 Internal Server Error";
    }
    return "400 Bad Request";
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    if (!reserve(req->content_len, NULL)) {
        return gw_http_error(req, "409 Conflict", "busy");
    }
    int total = req->content_len;
    bool force = query_force(req);
    ota_sess_t s = {0};
    const char *err = NULL;
    char *buf = malloc(CHUNK);
    if (!buf) {
        err = "no_mem";
        goto out;
    }

    /* 先收镜像头，检查工程名和版本，不合格就不擦写 flash */
    int n = recv_full(req, buf, CHUNK < total ? CHUNK : total);
    if (n < 0) {
        err = "recv_failed";
        goto out;
    }
    err = sess_begin(&s, buf, n, total, force);
    while (!err) {
        err = sess_write(&s, buf, n);
        int remain = total - s.received;
        if (err || remain <= 0) {
            break;
        }
        n = recv_full(req, buf, remain < CHUNK ? remain : CHUNK);
        if (n < 0) {
            err = "recv_failed";
        }
    }
    if (!err) {
        err = sess_finish(&s, NULL);
    }

out:
    free(buf);
    sess_abort(&s);
    if (err) {
        finish_fail(err);
        return gw_http_error(req, http_status_of(err), err);
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"version\":\"%s\",\"reboot_in_ms\":%d}", s_st.version, REBOOT_DELAY_MS);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    finish_ok();
    return ESP_OK;
}

/* ---------- 网关自己下载：远程升级 ---------- */

typedef struct {
    char *url;
    uint8_t sha[32];
    bool force;
} pull_req_t;

/* 读满 len 字节或读到结尾；出错返回 -1 */
static int http_read_full(esp_http_client_handle_t c, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = esp_http_client_read(c, buf + got, len - got);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            break;
        }
        got += r;
    }
    return got;
}

/* 打开连接并跟随跳转（gitee 附件会跳到另一个域名），返回内容长度，失败返回 NULL 错误以外的字符串 */
static const char *http_open(esp_http_client_handle_t c, int64_t *len)
{
    for (int i = 0; i <= PULL_MAX_REDIRECTS; i++) {
        if (esp_http_client_open(c, 0) != ESP_OK) {
            return "connect_failed";
        }
        *len = esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        if (code == 200) {
            return NULL;
        }
        if (code < 300 || code >= 400 || esp_http_client_set_redirection(c) != ESP_OK) {
            ESP_LOGW(TAG, "download http %d", code);
            return "download_http_error";
        }
        esp_http_client_close(c);
    }
    return "too_many_redirects";
}

static void log_heap(const char *where)
{
    ESP_LOGW(TAG, "%s: internal free %u (largest %u), psram free %u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void pull_task(void *arg)
{
    pull_req_t *req = arg;
    ota_sess_t s = {0};
    const char *err = NULL;
    char *buf = malloc(CHUNK);
    const esp_http_client_config_t cfg = {
        .url = req->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = PULL_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "esp-gw",
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!buf || !c) {
        log_heap("http client");
        err = "no_mem";
        goto out;
    }
    ESP_LOGI(TAG, "downloading %s", req->url);
    int64_t len = 0;
    err = http_open(c, &len);
    if (err) {
        goto out;
    }
    if (len <= 0) {
        err = "no_content_length";
        goto out;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.total = (uint32_t)len;
    xSemaphoreGive(s_lock);

    int n = http_read_full(c, buf, CHUNK < len ? CHUNK : (int)len);
    if (n < 0) {
        err = "download_failed";
        goto out;
    }
    err = sess_begin(&s, buf, n, (int)len, req->force);
    while (!err && n > 0) {
        err = sess_write(&s, buf, n);
        int remain = s.total - s.received;
        if (err || remain <= 0) {
            break;
        }
        n = http_read_full(c, buf, remain < CHUNK ? remain : CHUNK);
        if (n <= 0) {
            err = "download_failed";    /* 中途断开：已写的是空闲分区，丢弃即可 */
        }
    }
    if (!err) {
        err = sess_finish(&s, req->sha);
    }

out:
    sess_abort(&s);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(buf);
    free(req->url);
    free(req);
    if (err) {
        finish_fail(err);
    } else {
        finish_ok();
    }
    vTaskDelete(NULL);
}

static bool parse_sha256(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

const char *gw_ota_start_url(const char *url, const char *sha256_hex, const char *version, bool force)
{
    if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) > 512) {
        return "bad_url";
    }
    pull_req_t *req = calloc(1, sizeof(*req));
    if (!req) {
        return "no_mem";
    }
    if (!parse_sha256(sha256_hex, req->sha)) {
        free(req);
        return "bad_sha256";
    }
    req->url = strdup(url);
    req->force = force;
    if (!req->url) {
        free(req);
        return "no_mem";
    }
    if (!reserve(0, version)) {
        free(req->url);
        free(req);
        return "busy";
    }
    /* 栈必须在内部 RAM：任务里要写 flash（写 flash 时 PSRAM 不可访问） */
    if (xTaskCreate(pull_task, "ota_pull", PULL_STACK, req, 4, NULL) != pdPASS) {
        log_heap("ota task");
        free(req->url);
        free(req);
        finish_fail("no_mem");
        return "no_mem";
    }
    return NULL;
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

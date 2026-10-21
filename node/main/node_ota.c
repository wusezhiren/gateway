/*
 * 节点固件升级（网关经 ESP-NOW 分片推送），在 link 任务里调用，不另开任务。
 *
 * 防变砖：只写空闲 OTA 分区，bootloader / 分区表 / 正在运行的分区都不碰；
 * END 时核对 SHA-256、esp_ota_end 校验镜像和签名、工程名一致、版本更新，全部通过才切换启动分区。
 * 新固件首次启动要连上网关才确认有效，否则回滚（见 node_link.c）。
 */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "node_ota.h"

static const char *TAG = "node_ota";

#define IDLE_TIMEOUT_US  (30 * 1000000LL)  /* 这么久没收到升级帧就放弃 */
#define REBOOT_DELAY_US  (500 * 1000)

typedef enum { S_IDLE, S_RECEIVING, S_DONE } state_t;

static state_t s_state;
static const esp_partition_t *s_part;
static esp_ota_handle_t s_h;
static uint32_t s_size;
static uint32_t s_received;
static uint8_t s_sha[32];
static bool s_force;
static int s_last_pct;
static int64_t s_last_us;
static mbedtls_sha256_context s_ctx;
static esp_timer_handle_t s_reboot_timer;

static void send_ack(const uint8_t gw[6], uint16_t seq, uint8_t status)
{
    mp_ota_ack_t ack = {.offset = s_received, .status = status};
    mp_hdr_init(&ack.hdr, MP_OTA_ACK, seq);
    esp_now_send(gw, (const uint8_t *)&ack, sizeof(ack));
}

static void abort_session(const char *why)
{
    if (s_state == S_RECEIVING) {
        esp_ota_abort(s_h);
        mbedtls_sha256_free(&s_ctx);
        ESP_LOGW(TAG, "update aborted: %s (%lu/%lu)", why, (unsigned long)s_received, (unsigned long)s_size);
    }
    s_state = S_IDLE;
    s_received = 0;
}

static void reboot_cb(void *arg)
{
    ESP_LOGW(TAG, "rebooting into new firmware");
    esp_restart();
}

static uint8_t on_begin(const mp_ota_begin_t *b)
{
    abort_session("restarted");
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part || b->size == 0 || b->size > s_part->size) {
        return MP_OTA_ERR_SIZE;
    }
    /* 顺序写入：边写边擦，不会一上来阻塞几秒擦整个分区 */
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        return MP_OTA_ERR_BEGIN;
    }
    s_size = b->size;
    memcpy(s_sha, b->sha256, sizeof(s_sha));
    s_force = b->flags & MP_OTA_F_FORCE;
    s_received = 0;
    s_last_pct = 0;
    mbedtls_sha256_init(&s_ctx);
    mbedtls_sha256_starts(&s_ctx, 0);
    s_state = S_RECEIVING;
    char ver[MP_VER_LEN + 1] = {0};
    memcpy(ver, b->version, MP_VER_LEN);
    ESP_LOGI(TAG, "receiving v%s (%lu bytes) -> %s%s", ver, (unsigned long)s_size, s_part->label,
             s_force ? " (forced)" : "");
    return MP_OTA_OK;
}

/* 返回 true 表示要回 ACK */
static bool on_data(const mp_ota_data_t *d, int len, uint8_t *status)
{
    if (s_state != S_RECEIVING) {
        *status = MP_OTA_ERR_NO_SESSION;
        return true;
    }
    *status = MP_OTA_OK;
    bool ack_req = d->flags & MP_OTA_F_ACK_REQ;
    if (d->len == 0 || d->len > MP_OTA_CHUNK || len < MP_OTA_DATA_HDR_LEN + d->len) {
        return ack_req;
    }
    /* 只接受紧接着的那片；乱序或重复的丢掉，网关从 ACK 的 offset 重发 */
    if (d->offset != s_received || s_received + d->len > s_size) {
        return ack_req;
    }
    if (esp_ota_write(s_h, d->data, d->len) != ESP_OK) {
        abort_session("flash write failed");
        *status = MP_OTA_ERR_WRITE;
        return true;
    }
    mbedtls_sha256_update(&s_ctx, d->data, d->len);
    s_received += d->len;
    int pct = (int)((uint64_t)s_received * 100 / s_size);
    if (pct >= s_last_pct + 10) {
        s_last_pct = pct;
        ESP_LOGI(TAG, "%d%%", pct);
    }
    return ack_req || s_received == s_size;
}

static uint8_t on_end(void)
{
    if (s_state == S_DONE) {
        return MP_OTA_OK;       /* 网关没收到上一次的 ACK，重发了 END */
    }
    if (s_state != S_RECEIVING) {
        return MP_OTA_ERR_NO_SESSION;
    }
    if (s_received != s_size) {
        return MP_OTA_ERR_INCOMPLETE;
    }
    uint8_t sha[32];
    mbedtls_sha256_finish(&s_ctx, sha);
    mbedtls_sha256_free(&s_ctx);
    s_state = S_IDLE;
    if (memcmp(sha, s_sha, sizeof(sha)) != 0) {
        esp_ota_abort(s_h);
        ESP_LOGE(TAG, "sha256 mismatch");
        return MP_OTA_ERR_SHA256;
    }
    esp_err_t err = esp_ota_end(s_h);   /* 校验镜像完整性、芯片型号和签名 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image verify failed: %s", esp_err_to_name(err));
        return MP_OTA_ERR_IMAGE;
    }
    esp_app_desc_t desc;
    const esp_app_desc_t *cur = esp_app_get_description();
    if (esp_ota_get_partition_description(s_part, &desc) != ESP_OK) {
        return MP_OTA_ERR_IMAGE;
    }
    if (strncmp(desc.project_name, cur->project_name, sizeof(desc.project_name)) != 0) {
        ESP_LOGE(TAG, "wrong project %s", desc.project_name);
        return MP_OTA_ERR_PROJECT;
    }
    if (!s_force && node_ota_version_cmp(desc.version, cur->version) <= 0) {
        ESP_LOGE(TAG, "v%s is not newer than v%s", desc.version, cur->version);
        return MP_OTA_ERR_NOT_NEWER;
    }
    if (esp_ota_set_boot_partition(s_part) != ESP_OK) {
        return MP_OTA_ERR_IMAGE;
    }
    ESP_LOGI(TAG, "v%s verified, boot partition -> %s", desc.version, s_part->label);
    s_state = S_DONE;
    /* 留点时间让 ACK 发出去 */
    const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "ota_reboot"};
    if (esp_timer_create(&args, &s_reboot_timer) == ESP_OK) {
        esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_US);
    } else {
        esp_restart();
    }
    return MP_OTA_OK;
}

void node_ota_handle(int type, const uint8_t *data, int len, const uint8_t gw[6])
{
    const mp_hdr_t *hdr = (const mp_hdr_t *)data;
    uint8_t status = MP_OTA_OK;
    s_last_us = esp_timer_get_time();
    switch (type) {
    case MP_OTA_BEGIN:
        status = on_begin((const mp_ota_begin_t *)data);
        send_ack(gw, hdr->seq, status);
        break;
    case MP_OTA_DATA:
        if (on_data((const mp_ota_data_t *)data, len, &status)) {
            send_ack(gw, hdr->seq, status);
        }
        break;
    case MP_OTA_END:
        status = on_end();
        send_ack(gw, hdr->seq, status);
        break;
    case MP_OTA_ABORT:
        abort_session("cancelled by gateway");
        break;
    default:
        break;
    }
}

void node_ota_cancel(void)
{
    abort_session("link lost");
}

void node_ota_poll(void)
{
    if (s_state == S_RECEIVING && esp_timer_get_time() - s_last_us > IDLE_TIMEOUT_US) {
        abort_session("timeout");
    }
}

bool node_ota_active(void)
{
    return s_state == S_RECEIVING;
}

int node_ota_version_cmp(const char *a, const char *b)
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

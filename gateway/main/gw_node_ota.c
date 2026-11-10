/*
 * 节点固件升级：固件先暂存到 node_fw 分区（网关自己 HTTPS 下载，或局域网上传），
 * 核对后经 ESP-NOW 分片推给节点，再等节点以新版本重新入网。一次只升级一个节点。
 *
 * 防变砖（节点侧见 node/main/node_ota.c）：
 *  - 网关先核对镜像头：芯片型号、工程名与节点一致，版本更新；下载的再核对 SHA-256
 *  - 节点只写空闲 OTA 分区，END 时核对 SHA-256 + 签名 + 工程名 + 版本才切换启动分区
 *  - 新固件首次启动连不上网关就自动回滚，网关据此报 rolled_back
 *  - 传输中断、节点掉线、网关重启都只让这次升级作废，节点照常运行旧固件
 */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_sha256.h"
#include "gw_http.h"
#include "gw_link.h"
#include "gw_node_ota.h"
#include "gw_ota.h"

static const char *TAG = "gw_node_ota";

#define STAGE_SUBTYPE    0x40
#define STAGE_LABEL      "node_fw"
#define SECTOR           4096
#define CHUNK            4096
#define BURST            8          /* 每批连发片数，最后一片要求 ACK */
#define BEGIN_TIMEOUT_MS 1000
#define ACK_TIMEOUT_MS   500
#define END_TIMEOUT_MS   5000       /* 节点要校验签名 */
#define REQ_TRIES        5
#define MAX_FAILS        20         /* 连续这么多批没进展就放弃 */
#define REJOIN_TIMEOUT_US (150 * 1000000LL)    /* 节点确认超时 60 秒回滚后也能回来 */
#define TASK_STACK       8192
#define RECV_RETRIES     5
#define DESC_OFFSET      (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEAD_NEED        (DESC_OFFSET + sizeof(esp_app_desc_t))

typedef struct {
    uint8_t mac[6];
    char *url;          /* NULL：固件已上传到暂存区 */
    uint8_t sha[32];
    bool force;
} job_t;

typedef struct {
    uint8_t src[6];
    mp_ota_ack_t ack;
} ack_msg_t;

/* 暂存区内容（只在升级任务或上传请求里改，reserve 保证同一时间只有一个） */
typedef struct {
    bool valid;
    uint32_t size;
    uint8_t sha[32];
} staged_t;

typedef struct {
    uint32_t total;
    uint32_t written;
    uint32_t erased;
    int last_pct;
    gw_sha256_ctx_t sha;
} stage_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_ack_q;
static const esp_partition_t *s_part;
static gw_node_ota_status_t s_st;
static gw_node_ota_cb_t s_cb;
static staged_t s_staged;
static uint16_t s_seq;

const char *gw_node_ota_state_name(gw_node_ota_state_t s)
{
    static const char *const names[] = {"idle", "downloading", "sending", "rebooting", "done", "failed"};
    return s <= GW_NODE_OTA_FAILED ? names[s] : "?";
}

static void notify(void)
{
    if (s_cb) {
        gw_node_ota_status_t st;
        gw_node_ota_get_status(&st);
        s_cb(&st);
    }
}

static void set_state(gw_node_ota_state_t state, uint32_t total, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = state;
    s_st.received = 0;
    s_st.total = total;
    strlcpy(s_st.error, error ? error : "", sizeof(s_st.error));
    xSemaphoreGive(s_lock);
    notify();
}

/* 进度每 5% 通知一次 */
static void set_progress(uint32_t received, int *last_pct)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.received = received;
    int pct = s_st.total ? (int)((uint64_t)received * 100 / s_st.total) : 0;
    xSemaphoreGive(s_lock);
    if (pct >= *last_pct + 5) {
        *last_pct = pct;
        notify();
    }
}

static void set_version(const char *ver)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.version, ver, sizeof(s_st.version));
    xSemaphoreGive(s_lock);
}

bool gw_node_ota_busy(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_st.state >= GW_NODE_OTA_DOWNLOADING && s_st.state <= GW_NODE_OTA_REBOOTING;
    xSemaphoreGive(s_lock);
    return busy;
}

/* 占用升级通道：已有节点或网关升级在进行时返回错误码 */
static const char *reserve(const uint8_t mac[6])
{
    gw_ota_status_t gs;
    gw_ota_get_status(&gs);
    if (gs.state == GW_OTA_RECEIVING || gs.state == GW_OTA_REBOOTING) {
        return "busy";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_st.state >= GW_NODE_OTA_DOWNLOADING && s_st.state <= GW_NODE_OTA_REBOOTING;
    if (!busy) {
        memset(&s_st, 0, sizeof(s_st));
        s_st.state = GW_NODE_OTA_DOWNLOADING;
        memcpy(s_st.mac, mac, 6);
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        return "busy";
    }
    notify();
    return NULL;
}

static void finish(const char *err)
{
    if (err) {
        ESP_LOGW(TAG, "node ota failed: %s", err);
        set_state(GW_NODE_OTA_FAILED, 0, err);
    } else {
        set_state(GW_NODE_OTA_DONE, 0, NULL);
    }
}

/* ---------- 镜像检查 ---------- */

/* 镜像头里的芯片型号是 esp_chip_id_t，节点上报的是 esp_chip_model_t，C3/S3/C6 两者取值相同 */
static const char *check_image(const uint8_t *head, const gw_node_t *node, bool force)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)head;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(head + DESC_OFFSET);
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "not_an_app_image";
    }
    set_version(desc->version);
    if (ih->chip_id != node->chip) {
        return "wrong_chip";
    }
    /* 节点上报的工程名截断为 MP_PROJ_LEN */
    if (strncmp(desc->project_name, node->project, MP_PROJ_LEN) != 0) {
        return "wrong_project";
    }
    if (!force && gw_ota_version_cmp(desc->version, node->fw_version) <= 0) {
        return "not_newer";
    }
    return NULL;
}

static const char *find_node(const uint8_t mac[6], gw_node_t *node)
{
    if (!gw_link_get_node(mac, node, NULL)) {
        return "no_such_node";
    }
    return node->online ? NULL : "offline";
}

/* ---------- 暂存区写入：下载和上传共用，边写边擦 ---------- */

static const char *stage_begin(stage_t *s, uint32_t total)
{
    memset(s, 0, sizeof(*s));
    s_staged.valid = false;
    if (total < HEAD_NEED) {
        return "too_small";
    }
    if (total > s_part->size) {
        return "too_large";
    }
    s->total = total;
    gw_sha256_init(&s->sha);
    if (gw_sha256_starts(&s->sha) != 0) {
        gw_sha256_free(&s->sha);
        return "sha256_failed";
    }
    return NULL;
}

static const char *stage_write(stage_t *s, const uint8_t *buf, int n)
{
    if (s->written + n > s->total) {
        return "too_large";
    }
    while (s->erased < s->written + n) {
        if (esp_partition_erase_range(s_part, s->erased, SECTOR) != ESP_OK) {
            return "flash_erase_failed";
        }
        s->erased += SECTOR;
    }
    if (esp_partition_write(s_part, s->written, buf, n) != ESP_OK) {
        return "flash_write_failed";
    }
    if (gw_sha256_update(&s->sha, buf, n) != 0) {
        return "sha256_failed";
    }
    s->written += n;
    set_progress(s->written, &s->last_pct);
    return NULL;
}

/* expect 为 NULL 时不核对（局域网上传），sha_out 返回实际 SHA-256 */
static const char *stage_finish(stage_t *s, const uint8_t *expect, uint8_t sha_out[32])
{
    int hash_err = gw_sha256_finish(&s->sha, sha_out);
    gw_sha256_free(&s->sha);
    if (hash_err != 0) {
        return "sha256_failed";
    }
    if (s->written != s->total) {
        return "incomplete";
    }
    if (expect && memcmp(sha_out, expect, 32) != 0) {
        return "sha256_mismatch";
    }
    s_staged.size = s->total;
    memcpy(s_staged.sha, sha_out, 32);
    s_staged.valid = true;
    return NULL;
}

static void stage_abort(stage_t *s)
{
    if (s->total) {
        gw_sha256_free(&s->sha);
        s->total = 0;
    }
}

static const char *download(const job_t *job, const gw_node_t *node)
{
    stage_t s = {0};
    const char *err = NULL;
    uint8_t *buf = malloc(CHUNK);
    const esp_http_client_config_t cfg = {
        .url = job->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "esp-gw",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!buf || !c) {
        err = "no_mem";
        goto out;
    }
    ESP_LOGI(TAG, "downloading %s", job->url);
    int64_t len = 0;
    err = gw_ota_http_open(c, &len);
    if (err) {
        goto out;
    }
    if (len <= 0) {
        err = "no_content_length";
        goto out;
    }
    set_state(GW_NODE_OTA_DOWNLOADING, (uint32_t)len, NULL);
    err = stage_begin(&s, (uint32_t)len);
    while (!err && s.written < s.total) {
        uint32_t remain = s.total - s.written;
        int n = gw_ota_http_read(c, (char *)buf, remain < CHUNK ? remain : CHUNK);
        if (n <= 0) {
            err = "download_failed";
            break;
        }
        /* 先看镜像头，不是给这个节点的就不往下载了 */
        if (s.written == 0) {
            err = n < (int)HEAD_NEED ? "too_small" : check_image(buf, node, job->force);
            if (err) {
                break;
            }
        }
        err = stage_write(&s, buf, n);
    }
    if (!err) {
        uint8_t sha[32];
        err = stage_finish(&s, job->sha, sha);
    }
out:
    stage_abort(&s);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(buf);
    return err;
}

/* ---------- ESP-NOW 推送 ---------- */

static uint16_t next_seq(void)
{
    if (++s_seq == 0) {
        s_seq = 1;
    }
    return s_seq;
}

static esp_err_t send_frame(const uint8_t mac[6], const void *data, size_t len)
{
    esp_err_t err;
    /* 连发时 ESP-NOW 发送缓冲可能暂时满，稍等再发 */
    for (int i = 0; i < 50; i++) {
        err = esp_now_send(mac, data, len);
        if (err != ESP_ERR_ESPNOW_NO_MEM) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return err;
}

static bool wait_ack(const uint8_t mac[6], uint16_t seq, int timeout_ms, mp_ota_ack_t *out)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    ack_msg_t m;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 || xQueueReceive(s_ack_q, &m, deadline - now) != pdTRUE) {
            return false;
        }
        if (memcmp(m.src, mac, 6) == 0 && m.ack.hdr.seq == seq) {
            *out = m.ack;
            return true;
        }
    }
}

/* 发一个控制帧（BEGIN / END）并等 ACK，超时重发 */
static bool request(const uint8_t mac[6], mp_hdr_t *hdr, size_t len, int timeout_ms, mp_ota_ack_t *ack)
{
    for (int i = 0; i < REQ_TRIES; i++) {
        hdr->seq = next_seq();
        send_frame(mac, hdr, len);
        if (wait_ack(mac, hdr->seq, timeout_ms, ack)) {
            return true;
        }
    }
    return false;
}

static const char *status_name(uint8_t st)
{
    static const char *const names[] = {
        [MP_OTA_ERR_SIZE] = "node_too_large",       [MP_OTA_ERR_BEGIN] = "node_begin_failed",
        [MP_OTA_ERR_WRITE] = "node_write_failed",   [MP_OTA_ERR_NO_SESSION] = "node_no_session",
        [MP_OTA_ERR_INCOMPLETE] = "node_incomplete", [MP_OTA_ERR_SHA256] = "node_sha256_mismatch",
        [MP_OTA_ERR_IMAGE] = "invalid_image_or_signature", [MP_OTA_ERR_PROJECT] = "wrong_project",
        [MP_OTA_ERR_NOT_NEWER] = "not_newer",
    };
    return st < sizeof(names) / sizeof(names[0]) && names[st] ? names[st] : "node_error";
}

static const char *push(const gw_node_t *node, const char *version, bool force)
{
    const uint8_t *mac = node->mac;
    mp_ota_ack_t ack;
    xQueueReset(s_ack_q);
    set_state(GW_NODE_OTA_SENDING, s_staged.size, NULL);
    ESP_LOGI(TAG, "pushing v%s (%lu bytes) to " MACSTR, version, (unsigned long)s_staged.size, MAC2STR(mac));

    mp_ota_begin_t b = {.size = s_staged.size, .flags = force ? MP_OTA_F_FORCE : 0};
    mp_hdr_init(&b.hdr, MP_OTA_BEGIN, 0);
    memcpy(b.sha256, s_staged.sha, 32);
    memcpy(b.version, version, strnlen(version, MP_VER_LEN));
    if (!request(mac, &b.hdr, sizeof(b), BEGIN_TIMEOUT_MS, &ack)) {
        return "node_no_response";  /* 老固件不认识升级帧，也会走到这里 */
    }
    if (ack.status != MP_OTA_OK) {
        return status_name(ack.status);
    }

    static uint8_t buf[BURST * MP_OTA_CHUNK];
    static mp_ota_data_t f;
    uint32_t acked = 0;
    int fails = 0, last_pct = 0;
    int64_t t0 = esp_timer_get_time();
    while (acked < s_staged.size) {
        uint32_t n = s_staged.size - acked;
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        if (esp_partition_read(s_part, acked, buf, n) != ESP_OK) {
            goto abort_node;
        }
        uint16_t last_seq = 0;
        for (uint32_t off = 0; off < n; off += MP_OTA_CHUNK) {
            uint32_t len = n - off < MP_OTA_CHUNK ? n - off : MP_OTA_CHUNK;
            last_seq = next_seq();
            mp_hdr_init(&f.hdr, MP_OTA_DATA, last_seq);
            f.offset = acked + off;
            f.len = (uint8_t)len;
            f.flags = off + len >= n ? MP_OTA_F_ACK_REQ : 0;
            memcpy(f.data, buf + off, len);
            send_frame(mac, &f, MP_OTA_DATA_HDR_LEN + len);
        }
        if (wait_ack(mac, last_seq, ACK_TIMEOUT_MS, &ack)) {
            if (ack.status != MP_OTA_OK) {
                return status_name(ack.status);
            }
            if (ack.offset > acked && ack.offset <= s_staged.size) {
                acked = ack.offset;
                fails = 0;
                set_progress(acked, &last_pct);
                continue;
            }
        }
        if (++fails > MAX_FAILS) {
            ESP_LOGW(TAG, "no progress at %lu", (unsigned long)acked);
            goto abort_node;
        }
    }
    ESP_LOGI(TAG, "sent in %lld ms, waiting for node to verify",
             (esp_timer_get_time() - t0) / 1000);

    mp_ota_end_t e;
    mp_hdr_init(&e.hdr, MP_OTA_END, 0);
    if (request(mac, &e.hdr, sizeof(e), END_TIMEOUT_MS, &ack) && ack.status != MP_OTA_OK) {
        return status_name(ack.status);
    }
    /* 没收到 END 的 ACK 也可能是节点已经重启了，以是否带新版本回来为准 */
    return NULL;

abort_node: {
        mp_ota_abort_t a;
        mp_hdr_init(&a.hdr, MP_OTA_ABORT, next_seq());
        send_frame(mac, &a, sizeof(a));
        return "node_timeout";
    }
}

/* 等节点重启后重新入网：带着新版本回来才算成功 */
static const char *wait_rejoin(const uint8_t mac[6], uint32_t joins, const char *version)
{
    set_state(GW_NODE_OTA_REBOOTING, 0, NULL);
    int64_t deadline = esp_timer_get_time() + REJOIN_TIMEOUT_US;
    gw_node_t node;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!gw_link_get_node(mac, &node, NULL)) {
            return "no_such_node";
        }
        if (node.joins != joins) {
            if (strcmp(node.fw_version, version) == 0) {
                ESP_LOGI(TAG, "node " MACSTR " back with v%s", MAC2STR(mac), version);
                return NULL;
            }
            ESP_LOGW(TAG, "node " MACSTR " back with v%s, new firmware rolled back", MAC2STR(mac),
                     node.fw_version);
            return "rolled_back";
        }
    }
    return "no_rejoin";
}

/*
 * 深睡 / 省电节点：要求它保持唤醒（深睡节点不睡、省电节点一直收），等它下一次入网或心跳拿到 HB_ACK 里的标志；
 * out 更新为之后的节点信息
 */
static const char *wake_node(const uint8_t mac[6], gw_node_t *out)
{
    gw_node_t node;
    if (!gw_link_get_node(mac, &node, NULL)) {
        return "no_such_node";
    }
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + ((int64_t)node.sleep_s + MP_HB_INTERVAL_MS / 1000 + 10) * 1000000;
    ESP_LOGI(TAG, "node " MACSTR " sleeps %u s / rx window %u/%u ms, asking it to stay awake", MAC2STR(mac),
             node.sleep_s, node.lp_window_ms, node.lp_interval_ms);
    gw_link_stay_awake(mac, true);
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!gw_link_get_node(mac, &node, NULL)) {
            return "no_such_node";
        }
        if (node.last_seen_us > t0) {
            /* 深睡节点入网后紧接着发首个心跳；等它收到带保持唤醒标志的 HB_ACK */
            vTaskDelay(pdMS_TO_TICKS(500));
            return find_node(mac, out);
        }
    }
    return "node_asleep";
}

static const char *run_job(const job_t *job)
{
    gw_node_t node;
    const char *err = find_node(job->mac, &node);
    if (err) {
        return err;
    }
    if (job->url && !(s_staged.valid && memcmp(s_staged.sha, job->sha, 32) == 0)) {
        err = download(job, &node);
        if (err) {
            return err;
        }
    }
    if (!s_staged.valid) {
        return "no_image";
    }
    /* 暂存的固件可能是之前给别的节点下的，按这个节点再核对一次 */
    uint8_t head[HEAD_NEED];
    if (esp_partition_read(s_part, 0, head, sizeof(head)) != ESP_OK) {
        return "flash_read_failed";
    }
    err = check_image(head, &node, job->force);
    if (err) {
        return err;
    }
    char version[MP_VER_LEN + 1];
    strlcpy(version, ((const esp_app_desc_t *)(head + DESC_OFFSET))->version, sizeof(version));
    err = find_node(job->mac, &node);   /* 下载期间可能掉线 */
    if (!err) {
        /* 总是先要求保持唤醒、等它下一次心跳：网关重启后节点只靠心跳回来、不重新入网，
         * sleep_s / lp_interval_ms 没落盘，省电节点会被当成常醒，升级帧发在接收窗口外收不到 */
        err = wake_node(job->mac, &node);
    }
    if (err) {
        return err;
    }
    uint32_t joins = node.joins;
    err = push(&node, version, job->force);
    if (err) {
        return err;
    }
    return wait_rejoin(job->mac, joins, version);
}

static void job_task(void *arg)
{
    job_t *job = arg;
    const char *err = run_job(job);
    gw_link_stay_awake(job->mac, false);
    finish(err);
    free(job->url);
    free(job);
    vTaskDelete(NULL);
}

/* 栈必须在内部 RAM：任务里要写 flash */
static const char *spawn(job_t *job)
{
    if (xTaskCreate(job_task, "node_ota", TASK_STACK, job, 4, NULL) != pdPASS) {
        free(job->url);
        free(job);
        finish("no_mem");
        return "no_mem";
    }
    return NULL;
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

const char *gw_node_ota_start(const uint8_t mac[6], const char *url, const char *sha256_hex, bool force)
{
    if (!s_part) {
        return "no_stage_partition";
    }
    if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) > 512) {
        return "bad_url";
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return "no_mem";
    }
    if (!parse_sha256(sha256_hex, job->sha)) {
        free(job);
        return "bad_sha256";
    }
    gw_node_t node;
    const char *err = find_node(mac, &node);
    if (!err) {
        job->url = strdup(url);
        err = job->url ? NULL : "no_mem";
    }
    if (!err) {
        err = reserve(mac);
    }
    if (err) {
        free(job->url);
        free(job);
        return err;
    }
    memcpy(job->mac, mac, 6);
    job->force = force;
    return spawn(job);
}

/* ---------- 局域网上传：POST /api/ota/node?mac=xx:xx:xx:xx:xx:xx[&force=1] ---------- */

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

static esp_err_t upload_post(httpd_req_t *req)
{
    if (!gw_http_authorized(req)) {
        return ESP_OK;
    }
    char q[64], v[24];
    uint8_t mac[6];
    unsigned m[6];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "mac", v, sizeof(v)) != ESP_OK ||
        sscanf(v, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return gw_http_error(req, "400 Bad Request", "bad_mac");
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)m[i];
    }
    bool force = httpd_query_key_value(q, "force", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;

    gw_node_t node;
    const char *err = s_part ? find_node(mac, &node) : "no_stage_partition";
    if (err) {
        return gw_http_error(req, strcmp(err, "no_such_node") == 0 ? "404 Not Found" : "409 Conflict", err);
    }
    err = reserve(mac);
    if (err) {
        return gw_http_error(req, "409 Conflict", err);
    }
    set_state(GW_NODE_OTA_DOWNLOADING, req->content_len, NULL);

    stage_t s = {0};
    char *buf = malloc(CHUNK);
    err = buf ? stage_begin(&s, req->content_len) : "no_mem";
    while (!err && s.written < s.total) {
        uint32_t remain = s.total - s.written;
        int n = recv_full(req, buf, remain < CHUNK ? remain : CHUNK);
        if (n <= 0) {
            err = "recv_failed";
            break;
        }
        if (s.written == 0) {
            err = n < (int)HEAD_NEED ? "too_small" : check_image((const uint8_t *)buf, &node, force);
            if (err) {
                break;
            }
        }
        err = stage_write(&s, (const uint8_t *)buf, n);
    }
    uint8_t sha[32];
    if (!err) {
        err = stage_finish(&s, NULL, sha);
    }
    stage_abort(&s);
    free(buf);
    job_t *job = err ? NULL : calloc(1, sizeof(*job));
    if (!err && !job) {
        err = "no_mem";
    }
    if (err) {
        finish(err);
        return gw_http_error(req, strcmp(err, "not_newer") == 0 ? "409 Conflict" : "400 Bad Request", err);
    }
    memcpy(job->mac, mac, 6);
    memcpy(job->sha, sha, 32);
    job->force = force;
    err = spawn(job);
    if (err) {
        return gw_http_error(req, "500 Internal Server Error", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------- 对外 ---------- */

void gw_node_ota_on_ack(const uint8_t src[6], const uint8_t *data, int len)
{
    if (!s_ack_q || len < (int)sizeof(mp_ota_ack_t)) {
        return;
    }
    ack_msg_t m;
    memcpy(m.src, src, 6);
    memcpy(&m.ack, data, sizeof(m.ack));
    xQueueSend(s_ack_q, &m, 0);
}

esp_err_t gw_node_ota_register(httpd_handle_t httpd)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_ack_q = xQueueCreate(8, sizeof(ack_msg_t));
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, STAGE_SUBTYPE, STAGE_LABEL);
        if (!s_lock || !s_ack_q) {
            return ESP_ERR_NO_MEM;
        }
        if (!s_part) {
            ESP_LOGE(TAG, "no %s partition, node ota disabled", STAGE_LABEL);
        }
    }
    const httpd_uri_t uri = {.uri = "/api/ota/node", .method = HTTP_POST, .handler = upload_post};
    return httpd_register_uri_handler(httpd, &uri);
}

void gw_node_ota_set_cb(gw_node_ota_cb_t cb)
{
    s_cb = cb;
}

void gw_node_ota_get_status(gw_node_ota_status_t *st)
{
    if (!s_lock) {
        memset(st, 0, sizeof(*st));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *st = s_st;
    xSemaphoreGive(s_lock);
}

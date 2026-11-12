/*
 * 网关侧 ESP-NOW 链路：节点表、配对、命令下发与重传、在线检测。
 * 深睡节点（JOIN_REQ 带 sleep_s）每次醒来都重新入网：离线判定放宽一个睡眠周期，
 * SET 重试耗尽不算失败，期望值留着等它下次入网补发。
 * 省电节点（JOIN_REQ 带 lp_interval_ms）只在接收窗口里收得到：SET 每半个窗口重发一次，持续一个周期多。
 * 所有节点表访问都持有 s_lock（递归锁，事件回调里可以再调 snapshot）。
 */
#include <string.h>
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "gw_link.h"
#include "gw_set_policy.h"
#include "gw_node_ota.h"

static const char *TAG = "gw_link";

#define NVS_NS              "gw_nodes"
#define NVS_KEY             "table_v1"
#define NVS_KEY_RENAMED     "renamed"   /* 用户改过名的节点位图，单独存，不动 table_v1 的格式 */
#define SET_TIMEOUT_US      (300 * 1000)
#define SET_MAX_TRIES       3
#define OFFLINE_TIMEOUT_US  ((int64_t)(MP_HB_MISS_MAX * MP_HB_INTERVAL_MS + 1000) * 1000)
#define TICK_MS             10      /* 省电节点的 SET 重发间隔只有几十毫秒 */

typedef struct {
    uint8_t  src[6];
    int8_t   rssi;
    uint8_t  len;
    uint8_t  data[ESP_NOW_MAX_DATA_LEN];
} rx_msg_t;

typedef struct {
    bool     active;
    uint16_t seq;
    uint16_t value;
    uint8_t  tries;
    int64_t  next_us;
} pending_t;

/* 落盘部分，结构变更时需要换 NVS_KEY */
typedef struct __attribute__((packed)) {
    uint8_t      used;
    uint8_t      mac[6];
    char         name[MP_NAME_LEN + 1];
    char         fw_version[MP_VER_LEN + 1];
    char         project[MP_PROJ_LEN + 1];
    uint8_t      chip;
    uint8_t      n_ch;
    mp_ch_desc_t ch[MP_MAX_CH];
} persist_t;

static gw_node_t s_nodes[GW_MAX_NODES];
static pending_t s_pending[GW_MAX_NODES][MP_MAX_CH];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_rx_q;
static gw_event_cb_t s_event_cb;
static int64_t s_pair_until_us;
static uint16_t s_seq;
static uint32_t s_renamed;  /* BIT(idx)：名字是用户起的，入网时不用节点上报的覆盖 */

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

static void emit(gw_event_t ev, int idx)
{
    if (s_event_cb) {
        s_event_cb(ev, idx);
    }
}

static uint16_t next_seq(void)
{
    if (++s_seq == 0) {     /* 0 保留给“非应答” */
        s_seq = 1;
    }
    return s_seq;
}

/* 拷贝定长、可能无结尾 0 的字段，dst 至少 n+1 字节 */
static void copy_field(char *dst, const char *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int find_node(const uint8_t mac[6])
{
    for (int i = 0; i < GW_MAX_NODES; i++) {
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static void save_table(void)
{
    static persist_t buf[GW_MAX_NODES];
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < GW_MAX_NODES; i++) {
        const gw_node_t *n = &s_nodes[i];
        if (!n->used) {
            continue;
        }
        buf[i].used = 1;
        memcpy(buf[i].mac, n->mac, 6);
        memcpy(buf[i].name, n->name, sizeof(buf[i].name));
        memcpy(buf[i].fw_version, n->fw_version, sizeof(buf[i].fw_version));
        memcpy(buf[i].project, n->project, sizeof(buf[i].project));
        buf[i].chip = n->chip;
        buf[i].n_ch = n->n_ch;
        memcpy(buf[i].ch, n->ch, sizeof(buf[i].ch));
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, buf, sizeof(buf));
        if (err == ESP_OK) {
            err = nvs_set_u32(h, NVS_KEY_RENAMED, s_renamed);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save node table failed: %s", esp_err_to_name(err));
    }
}

static void load_table(void)
{
    static persist_t buf[GW_MAX_NODES];
    size_t len = sizeof(buf);
    nvs_handle_t h;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_get_blob(h, NVS_KEY, buf, &len);
    if (nvs_get_u32(h, NVS_KEY_RENAMED, &s_renamed) != ESP_OK) {
        s_renamed = 0;
    }
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(buf)) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "node table size mismatch (GW_MAX_NODES changed?), ignored");
        }
        return;
    }

    for (int i = 0; i < GW_MAX_NODES; i++) {
        if (!buf[i].used) {
            continue;
        }
        gw_node_t *n = &s_nodes[i];
        n->used = true;
        memcpy(n->mac, buf[i].mac, 6);
        memcpy(n->name, buf[i].name, sizeof(n->name));
        n->name[MP_NAME_LEN] = '\0';
        copy_field(n->fw_version, buf[i].fw_version, MP_VER_LEN);
        copy_field(n->project, buf[i].project, MP_PROJ_LEN);
        n->chip = buf[i].chip;
        n->n_ch = buf[i].n_ch > MP_MAX_CH ? MP_MAX_CH : buf[i].n_ch;
        memcpy(n->ch, buf[i].ch, sizeof(n->ch));
        if (mp_peer_upsert(n->mac, true) != ESP_OK) {
            ESP_LOGE(TAG, "add peer " MACSTR " failed", MAC2STR(n->mac));
        }
        ESP_LOGI(TAG, "loaded node %d " MACSTR " \"%s\"", i, MAC2STR(n->mac), n->name);
    }
}

static uint8_t current_channel(void)
{
    uint8_t ch = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&ch, &second);
    return ch;
}

static int64_t set_timeout_us(const gw_node_t *n)
{
    return gw_set_retry_interval_us(n->lp_interval_ms, n->lp_window_ms);
}

static int set_max_tries(const gw_node_t *n)
{
    return gw_set_retry_limit(n->lp_interval_ms, n->lp_window_ms);
}

static void send_set(int idx, uint8_t ch)
{
    pending_t *p = &s_pending[idx][ch];
    mp_set_t msg;
    mp_hdr_init(&msg.hdr, MP_SET, p->seq);
    msg.ch = ch;
    msg.value = p->value;
    p->tries++;
    p->next_us = esp_timer_get_time() + set_timeout_us(&s_nodes[idx]);
    esp_err_t err = esp_now_send(s_nodes[idx].mac, (const uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send SET to node %d failed: %s", idx, esp_err_to_name(err));
    }
}

/* 发起一次带重传的 SET；同一通道的新命令覆盖未完成的旧命令 */
static void start_set(int idx, uint8_t ch, uint16_t value)
{
    pending_t *p = &s_pending[idx][ch];
    p->active = true;
    p->seq = next_seq();
    p->value = value;
    p->tries = 0;
    send_set(idx, ch);
}

static void clear_pending(int idx)
{
    memset(s_pending[idx], 0, sizeof(s_pending[idx]));
}

static void handle_join(const rx_msg_t *m)
{
    const mp_join_req_t *req = (const mp_join_req_t *)m->data;
    bool is_new = false;
    bool pairing = esp_timer_get_time() < s_pair_until_us;

    LOCK();
    int idx = find_node(m->src);
    if (idx < 0) {
        if (!pairing) {
            UNLOCK();
            ESP_LOGD(TAG, "ignore JOIN from " MACSTR " (not pairing)", MAC2STR(m->src));
            return;
        }
        for (int i = 0; i < GW_MAX_NODES; i++) {
            if (!s_nodes[i].used) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            UNLOCK();
            ESP_LOGW(TAG, "node table full, reject " MACSTR, MAC2STR(m->src));
            return;
        }
        is_new = true;
        s_renamed &= ~BIT(idx);
        memset(&s_nodes[idx], 0, sizeof(s_nodes[idx]));
        s_nodes[idx].used = true;
        memcpy(s_nodes[idx].mac, m->src, 6);
    }

    gw_node_t *n = &s_nodes[idx];
    char name[MP_NAME_LEN + 1];
    char ver[MP_VER_LEN + 1];
    char proj[MP_PROJ_LEN + 1];
    if (s_renamed & BIT(idx)) {
        strcpy(name, n->name);
    } else {
        copy_field(name, req->name, MP_NAME_LEN);
    }
    copy_field(ver, req->fw_version, MP_VER_LEN);
    copy_field(proj, req->project, MP_PROJ_LEN);
    uint8_t n_ch = req->n_ch > MP_MAX_CH ? MP_MAX_CH : req->n_ch;
    uint16_t sleep_s = MP_JOIN_REQ_HAS(m->len, sleep_s) ? req->sleep_s : 0;
    bool lp = MP_JOIN_REQ_HAS(m->len, lp_window_ms);

    bool layout_changed = is_new || n->n_ch != n_ch ||
                          memcmp(n->ch, req->ch, sizeof(mp_ch_desc_t) * n_ch) != 0;
    bool changed = layout_changed || strcmp(n->name, name) != 0 || strcmp(n->fw_version, ver) != 0 ||
                   strcmp(n->project, proj) != 0 || n->chip != req->chip;
    if (layout_changed) {
        /* 通道布局变了，旧的期望值没有意义 */
        n->n_ch = n_ch;
        memset(n->ch, 0, sizeof(n->ch));
        memcpy(n->ch, req->ch, sizeof(mp_ch_desc_t) * n_ch);
        memset(n->value, 0, sizeof(n->value));
        n->desired_mask = 0;
    }
    if (changed) {
        strcpy(n->name, name);
        strcpy(n->fw_version, ver);
        strcpy(n->project, proj);
        n->chip = req->chip;
        save_table();
    }
    /* 深睡节点每个周期都入网一次，没变化就不打日志、不发事件 */
    bool quiet = !changed && n->online && sleep_s;
    n->resync = true;
    n->joins++;
    n->sleep_s = sleep_s;
    n->lp_interval_ms = lp ? req->lp_interval_ms : 0;
    n->lp_window_ms = lp ? req->lp_window_ms : 0;

    if (mp_peer_upsert(n->mac, true) != ESP_OK) {
        ESP_LOGE(TAG, "add peer " MACSTR " failed", MAC2STR(n->mac));
    }
    n->online = true;
    n->rssi = m->rssi;
    n->last_seen_us = esp_timer_get_time();
    clear_pending(idx);

    /* 节点此时还没把网关加为加密 peer，应答走广播 */
    mp_join_rsp_t rsp = {
        .status = MP_JOIN_OK,
        .channel = current_channel(),
        .hb_interval_ms = MP_HB_INTERVAL_MS,
    };
    mp_hdr_init(&rsp.hdr, MP_JOIN_RSP, req->hdr.seq);
    memcpy(rsp.target, m->src, 6);
    esp_now_send(MP_BROADCAST, (const uint8_t *)&rsp, sizeof(rsp));

    if (quiet) {
        ESP_LOGD(TAG, "node %d woke up, rssi %d", idx, m->rssi);
    } else {
        ESP_LOGI(TAG, "%s node %d " MACSTR " \"%s\" %s v%s, %d ch, rssi %d, sleep %u s, rx window %u/%u ms",
                 is_new ? "paired" : "rejoined", idx, MAC2STR(n->mac), n->name, n->project,
                 n->fw_version, n->n_ch, m->rssi, sleep_s, n->lp_window_ms, n->lp_interval_ms);
        emit(GW_EV_NODE_JOINED, idx);
    }
    UNLOCK();
}

static void handle_state(const rx_msg_t *m, int type)
{
    const mp_state_t *st = (const mp_state_t *)m->data;

    LOCK();
    int idx = find_node(m->src);
    if (idx < 0) {
        UNLOCK();
        return;
    }
    gw_node_t *n = &s_nodes[idx];
    bool was_online = n->online;
    n->online = true;
    n->rssi = m->rssi;
    n->uptime_s = st->uptime_s;
    n->last_seen_us = esp_timer_get_time();

    bool changed = false;
    uint8_t cnt = st->n_ch < n->n_ch ? st->n_ch : n->n_ch;
    for (int i = 0; i < cnt; i++) {
        if (n->value[i] != st->value[i]) {
            n->value[i] = st->value[i];
            changed = true;
        }
    }

    /* A transport ACK alone does not prove that the output changed. Preserve
     * the requested value until the node reports the matching actual state. */
    for (int i = 0; i < cnt; ++i) {
        if (s_pending[idx][i].active && gw_set_confirmed(n->value[i], s_pending[idx][i].value))
            s_pending[idx][i].active = false;
    }

    if (n->resync) {
        /* 刚入网：实际值与期望值不符（如节点重启后状态丢失）则补发 */
        n->resync = false;
        for (int i = 0; i < n->n_ch; i++) {
            if ((n->desired_mask & BIT(i)) && n->value[i] != n->desired[i]) {
                ESP_LOGI(TAG, "node %d ch %d is %u, restore %u", idx, i, n->value[i], n->desired[i]);
                start_set(idx, i, n->desired[i]);
            }
        }
    } else {
        /* 没有命令在途时，以节点上报为准（节点本地改动也能同步过来） */
        for (int i = 0; i < n->n_ch; i++) {
            if (!s_pending[idx][i].active) {
                n->desired[i] = n->value[i];
                n->desired_mask |= BIT(i);
            }
        }
    }

    if (type == MP_HEARTBEAT) {
        mp_hb_ack_t ack = {.flags = n->stay_awake ? MP_HB_F_STAY_AWAKE : 0};
        mp_hdr_init(&ack.hdr, MP_HB_ACK, st->hdr.seq);
        esp_now_send(n->mac, (const uint8_t *)&ack, sizeof(ack));
        /* The node just transmitted inside its RX window. Retry pending SET
         * immediately instead of relying only on the periodic timer. */
        for (int ch = 0; ch < n->n_ch; ++ch) {
            if (s_pending[idx][ch].active) send_set(idx, ch);
        }
    }

    if (!was_online) {
        ESP_LOGI(TAG, "node %d \"%s\" back online", idx, n->name);
        emit(GW_EV_NODE_JOINED, idx);
    } else if (changed) {
        emit(GW_EV_NODE_STATE, idx);
    }
    UNLOCK();
}

static void periodic(void)
{
    int64_t now = esp_timer_get_time();

    LOCK();
    for (int i = 0; i < GW_MAX_NODES; i++) {
        gw_node_t *n = &s_nodes[i];
        if (!n->used || !n->online) {
            continue;
        }
        if (now - n->last_seen_us > OFFLINE_TIMEOUT_US + (int64_t)n->sleep_s * 1000000) {
            n->online = false;
            clear_pending(i);
            ESP_LOGW(TAG, "node %d \"%s\" offline", i, n->name);
            emit(GW_EV_NODE_OFFLINE, i);
            continue;
        }
        for (int ch = 0; ch < n->n_ch; ch++) {
            pending_t *p = &s_pending[i][ch];
            if (!p->active || now < p->next_us) {
                continue;
            }
            if (p->tries >= set_max_tries(n)) {
                /* Keep the user's desired value until node state confirms it.
                 * Otherwise the next heartbeat overwrites desired with the
                 * stale actual value and a failed OFF command turns back ON. */
                p->tries = 0;
                p->next_us = now + (int64_t)(n->lp_interval_ms ? n->lp_interval_ms : 1000) * 1000;
                if (n->sleep_s) {
                    ESP_LOGI(TAG, "node %d asleep, ch %d = %u queued until it wakes", i, ch, p->value);
                } else {
                    ESP_LOGW(TAG, "node %d ch %d SET %u not acked", i, ch, p->value);
                    emit(GW_EV_SET_FAILED, i);
                }
            } else {
                send_set(i, ch);
            }
        }
    }
    UNLOCK();
}

static void link_task(void *arg)
{
    static rx_msg_t m;
    for (;;) {
        if (xQueueReceive(s_rx_q, &m, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            int type = mp_parse(m.data, m.len);
            switch (type) {
            case MP_JOIN_REQ:
                handle_join(&m);
                break;
            case MP_HEARTBEAT:
            case MP_STATE:
                handle_state(&m, type);
                break;
            case MP_OTA_ACK:
                gw_node_ota_on_ack(m.src, m.data, m.len);
                break;
            default:
                break;
            }
        }
        periodic();
    }
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    rx_msg_t m;
    if (len <= 0 || len > ESP_NOW_MAX_DATA_LEN) {
        return;
    }
    memcpy(m.src, info->src_addr, 6);
    m.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    m.len = len;
    memcpy(m.data, data, len);
    if (xQueueSend(s_rx_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, drop");
    }
}

esp_err_t gw_link_start(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_rx_q = xQueueCreate(16, sizeof(rx_msg_t));
    if (!s_lock || !s_rx_q) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(mp_espnow_setup());
    ESP_ERROR_CHECK(mp_peer_upsert(MP_BROADCAST, false));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    load_table();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "gateway " MACSTR " ready, channel %d", MAC2STR(mac), current_channel());

    if (xTaskCreate(link_task, "gw_link", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gw_link_set_event_cb(gw_event_cb_t cb)
{
    s_event_cb = cb;
}

void gw_link_snapshot(gw_node_t out[GW_MAX_NODES])
{
    LOCK();
    memcpy(out, s_nodes, sizeof(s_nodes));
    UNLOCK();
}

bool gw_link_get_node(const uint8_t mac[6], gw_node_t *out, int *idx)
{
    LOCK();
    int i = find_node(mac);
    if (i >= 0) {
        *out = s_nodes[i];
        if (idx) {
            *idx = i;
        }
    }
    UNLOCK();
    return i >= 0;
}

esp_err_t gw_link_set(int idx, uint8_t ch, uint16_t value)
{
    if (idx < 0 || idx >= GW_MAX_NODES) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    gw_node_t *n = &s_nodes[idx];
    esp_err_t err = ESP_OK;
    if (!n->used || ch >= n->n_ch || value > n->ch[ch].max) {
        err = ESP_ERR_INVALID_ARG;
    } else if (!n->online) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        n->desired[ch] = value;
        n->desired_mask |= BIT(ch);
        start_set(idx, ch, value);
    }
    UNLOCK();
    return err;
}

esp_err_t gw_link_unpair(int idx)
{
    if (idx < 0 || idx >= GW_MAX_NODES) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    gw_node_t *n = &s_nodes[idx];
    if (!n->used) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    mp_leave_t msg;
    mp_hdr_init(&msg.hdr, MP_LEAVE, next_seq());
    esp_now_send(n->mac, (const uint8_t *)&msg, sizeof(msg));
    /* 给 LEAVE 留点发送时间再删 peer，否则帧还没发出去就丢了 */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_now_del_peer(n->mac);
    ESP_LOGI(TAG, "unpaired node %d " MACSTR, idx, MAC2STR(n->mac));
    memset(n, 0, sizeof(*n));
    s_renamed &= ~BIT(idx);
    clear_pending(idx);
    save_table();
    emit(GW_EV_NODE_REMOVED, idx);
    UNLOCK();
    return ESP_OK;
}

esp_err_t gw_link_rename(int idx, const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (idx < 0 || idx >= GW_MAX_NODES || len == 0 || len > MP_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    gw_node_t *n = &s_nodes[idx];
    if (!n->used) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    strcpy(n->name, name);
    s_renamed |= BIT(idx);
    save_table();
    ESP_LOGI(TAG, "renamed node %d " MACSTR " to \"%s\"", idx, MAC2STR(n->mac), n->name);
    emit(GW_EV_NODE_STATE, idx);
    UNLOCK();
    return ESP_OK;
}

void gw_link_stay_awake(const uint8_t mac[6], bool on)
{
    LOCK();
    int idx = find_node(mac);
    if (idx >= 0) {
        s_nodes[idx].stay_awake = on;
    }
    UNLOCK();
}

void gw_link_pair(uint32_t seconds)
{
    s_pair_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    if (seconds) {
        ESP_LOGI(TAG, "pairing open for %lu s", (unsigned long)seconds);
    } else {
        ESP_LOGI(TAG, "pairing closed");
    }
}

uint32_t gw_link_pair_remaining(void)
{
    int64_t left = s_pair_until_us - esp_timer_get_time();
    return left > 0 ? (uint32_t)((left + 999999) / 1000000) : 0;
}

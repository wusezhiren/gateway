/*
 * 节点侧 ESP-NOW 链路状态机：
 *   SCAN   逐信道广播 JOIN_REQ，收到发给自己的 JOIN_RSP 后入网（优先尝试上次的信道）
 *   JOINED 周期心跳；连续 MP_HB_MISS_MAX 次没收到 HB_ACK 回到 SCAN
 * 升级帧交给 node_ota.c 处理。
 * OTA 后首次启动：入网并收到首个 HB_ACK 才确认新固件有效，VALIDATE_TIMEOUT_US 内未完成则回滚。
 * 已绑定网关时只接受该网关的应答，收到 LEAVE 或长按复位键才清除绑定。
 * 深睡（NODE_SLEEP_INTERVAL_S）：通道全关时收到 HB_ACK 或找不到网关就深睡，定时醒来重新开机入网，
 * 网关在入网后的首个心跳里补发睡着期间的命令（SET 先于 HB_ACK 到达，开了灯就不再睡）。
 * 省电（NODE_LOW_POWER）：入网后每个周期只开一小段接收窗口，其余时间 CPU 自动浅睡；
 * 心跳在窗口开始时发，HB_ACK 才收得到。扫描、升级、网关要求保持唤醒时一直收。
 */
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "node_io.h"
#include "node_link.h"
#include "node_ota.h"

static const char *TAG = "node_link";

#define NVS_NS          "node"
#define JOIN_WAIT_MS    150
#define SCAN_PAUSE_MS   1000
#define MAX_CHANNEL     13
#define VALIDATE_TIMEOUT_US (60 * 1000 * 1000LL)
#define COLD_AWAKE_US   (30 * 1000 * 1000LL)    /* 上电后先醒着，方便 USB 烧录和长按复位键 */
#define STAY_AWAKE_US   (30 * 1000 * 1000LL)    /* 网关要求保持唤醒，每个 HB_ACK 续期 */

typedef struct {
    uint8_t src[6];
    uint8_t len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} rx_msg_t;

static QueueHandle_t s_rx_q;
static uint8_t s_my_mac[6];
static char s_name[MP_NAME_LEN + 1];
static mp_ch_desc_t s_desc[MP_MAX_CH];
static uint8_t s_n_ch;
static uint16_t s_seq;

static bool s_bound;            /* 是否已绑定网关 */
static uint8_t s_gw_mac[6];
static uint8_t s_gw_ch;
static uint16_t s_hb_ms = MP_HB_INTERVAL_MS;
static bool s_pending_verify;   /* 运行的是 OTA 后尚未确认的新固件 */
static esp_timer_handle_t s_validate_timer;
static int64_t s_stay_until_us;  /* 这之前不深睡、不开省电 */
#if CONFIG_NODE_LOW_POWER
static SemaphoreHandle_t s_window_sem;  /* 接收窗口开始 */
static bool s_lp_on;
#endif

static uint16_t next_seq(void)
{
    if (++s_seq == 0) {
        s_seq = 1;
    }
    return s_seq;
}

static void load_binding(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_gw_mac);
    s_bound = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "gw_mac", s_gw_mac, &len) == ESP_OK && len == 6 &&
        nvs_get_u8(h, "gw_ch", &s_gw_ch) == ESP_OK) {
        s_bound = true;
        ESP_LOGI(TAG, "bound to gateway " MACSTR " (last channel %d)", MAC2STR(s_gw_mac), s_gw_ch);
    }
    nvs_close(h);
}

static void save_binding(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "gw_mac", s_gw_mac, 6);
    nvs_set_u8(h, "gw_ch", s_gw_ch);
    nvs_commit(h);
    nvs_close(h);
}

void node_link_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_bound = false;
    ESP_LOGW(TAG, "gateway binding cleared");
}

static void send_state(mp_type_t type, uint16_t ack_seq)
{
    mp_state_t st = {
        .ack_seq = ack_seq,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .n_ch = s_n_ch,
    };
    mp_hdr_init(&st.hdr, type, next_seq());
    uint16_t value[MP_MAX_CH];
    node_io_get(value);
    memcpy(st.value, value, sizeof(st.value));  /* st 是 packed，不能直接传成员地址 */
    esp_err_t err = esp_now_send(s_gw_mac, (const uint8_t *)&st, sizeof(st));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send state failed: %s", esp_err_to_name(err));
    }
}

/* 在 channel 上请求入网，成功返回 true */
static bool try_join(uint8_t channel)
{
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    xQueueReset(s_rx_q);

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    mp_join_req_t req = {
        .chip = chip.model,
        .n_ch = s_n_ch,
    };
    uint16_t seq = next_seq();
    mp_hdr_init(&req.hdr, MP_JOIN_REQ, seq);
    /* 定长字段，超长截断且不要求结尾 0 */
    memcpy(req.fw_version, app->version, strnlen(app->version, MP_VER_LEN));
    memcpy(req.project, app->project_name, strnlen(app->project_name, MP_PROJ_LEN));
    memcpy(req.name, s_name, MP_NAME_LEN);
    memcpy(req.ch, s_desc, sizeof(req.ch));
    req.sleep_s = CONFIG_NODE_SLEEP_INTERVAL_S;
#if CONFIG_NODE_LOW_POWER
    req.lp_interval_ms = CONFIG_NODE_LP_INTERVAL_MS;
    req.lp_window_ms = CONFIG_NODE_LP_WINDOW_MS;
#endif
    esp_now_send(MP_BROADCAST, (const uint8_t *)&req, sizeof(req));

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(JOIN_WAIT_MS);
    rx_msg_t m;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 || xQueueReceive(s_rx_q, &m, deadline - now) != pdTRUE) {
            return false;
        }
        if (mp_parse(m.data, m.len) != MP_JOIN_RSP) {
            continue;
        }
        const mp_join_rsp_t *rsp = (const mp_join_rsp_t *)m.data;
        if (memcmp(rsp->target, s_my_mac, 6) != 0 || rsp->status != MP_JOIN_OK ||
            rsp->hdr.seq != seq) {
            continue;
        }
        if (s_bound && memcmp(m.src, s_gw_mac, 6) != 0) {
            ESP_LOGD(TAG, "ignore JOIN_RSP from foreign gateway " MACSTR, MAC2STR(m.src));
            continue;
        }

        uint8_t new_ch = rsp->channel ? rsp->channel : channel;
        bool dirty = !s_bound || new_ch != s_gw_ch;
        memcpy(s_gw_mac, m.src, 6);
        s_gw_ch = new_ch;
        s_bound = true;
        s_hb_ms = rsp->hb_interval_ms ? rsp->hb_interval_ms : MP_HB_INTERVAL_MS;
        if (s_gw_ch != channel) {
            esp_wifi_set_channel(s_gw_ch, WIFI_SECOND_CHAN_NONE);
        }
        if (dirty) {
            save_binding();
        }
        mp_peer_upsert(s_gw_mac, true);
        return true;
    }
}

/* 入网后且没有升级、网关没要求保持唤醒时才只开接收窗口，否则一直收 */
static void lp_update(bool joined)
{
#if CONFIG_NODE_LOW_POWER
    bool on = joined && !node_ota_active() && esp_timer_get_time() >= s_stay_until_us;
    if (on != s_lp_on) {
        s_lp_on = on;
        esp_now_set_wake_window(on ? CONFIG_NODE_LP_WINDOW_MS : UINT16_MAX);
        if (on) {
            ESP_LOGI(TAG, "rx window %d ms every %d ms", CONFIG_NODE_LP_WINDOW_MS, CONFIG_NODE_LP_INTERVAL_MS);
        } else {
            ESP_LOGI(TAG, "rx always on");
        }
    }
#endif
}

/* 省电时等到下一个接收窗口开始再发，网关的应答才落在窗口里 */
static void wait_rx_window(void)
{
#if CONFIG_NODE_LOW_POWER
    if (s_lp_on) {
        xSemaphoreTake(s_window_sem, 0);
        xSemaphoreTake(s_window_sem, pdMS_TO_TICKS(CONFIG_NODE_LP_INTERVAL_MS + 100));
    }
#endif
}

/* 条件满足就深睡，醒来从头开机，不返回 */
static void maybe_sleep(void)
{
#if CONFIG_NODE_SLEEP_INTERVAL_S > 0
    if (!s_bound || s_pending_verify || node_ota_active() || !node_io_all_off() ||
        esp_timer_get_time() < s_stay_until_us) {
        return;
    }
    ESP_LOGI(TAG, "all channels off, deep sleep %d s", CONFIG_NODE_SLEEP_INTERVAL_S);
    node_io_prepare_sleep();
    esp_wifi_stop();
    esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_NODE_SLEEP_INTERVAL_S * 1000000);
    esp_deep_sleep_start();
#endif
}

static void scan_until_joined(void)
{
    lp_update(false);
    for (;;) {
        if (s_bound && try_join(s_gw_ch)) {
            return;
        }
        for (uint8_t ch = 1; ch <= MAX_CHANNEL; ch++) {
            if (s_bound && ch == s_gw_ch) {
                continue;
            }
            if (try_join(ch)) {
                return;
            }
        }
        maybe_sleep();
        ESP_LOGI(TAG, "no gateway found, retry");
        vTaskDelay(pdMS_TO_TICKS(SCAN_PAUSE_MS));
    }
}

/* 返回时表示链路丢失或被解绑，需要重新扫描 */
static void run_joined(void)
{
    int missed = 0;
    TickType_t next_hb = xTaskGetTickCount();
    rx_msg_t m;

    for (;;) {
        lp_update(true);
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(next_hb - now) <= 0) {
            if (missed >= MP_HB_MISS_MAX) {
                ESP_LOGW(TAG, "gateway lost (%d heartbeats unanswered)", missed);
                return;
            }
            wait_rx_window();
            send_state(MP_HEARTBEAT, 0);
            missed++;
            next_hb += pdMS_TO_TICKS(s_hb_ms);
            continue;
        }
        node_ota_poll();
        if (xQueueReceive(s_rx_q, &m, next_hb - now) != pdTRUE) {
            continue;
        }
        if (memcmp(m.src, s_gw_mac, 6) != 0) {
            continue;
        }
        int type = mp_parse(m.data, m.len);
        switch (type) {
        case MP_HB_ACK:
            missed = 0;
            if (s_pending_verify) {
                s_pending_verify = false;
                esp_timer_stop(s_validate_timer);
                esp_ota_mark_app_valid_cancel_rollback();
                ESP_LOGI(TAG, "new firmware marked valid");
            }
            if (m.len >= sizeof(mp_hb_ack_t) && (((const mp_hb_ack_t *)m.data)->flags & MP_HB_F_STAY_AWAKE)) {
                s_stay_until_us = esp_timer_get_time() + STAY_AWAKE_US;
            }
            maybe_sleep();
            break;
        case MP_SET: {
            const mp_set_t *set = (const mp_set_t *)m.data;
            missed = 0;
            if (node_io_set(set->ch, set->value) != ESP_OK) {
                ESP_LOGW(TAG, "SET invalid ch %d", set->ch);
            }
            /* 无论成功与否都回实际状态，网关据此清除重传 */
            send_state(MP_STATE, set->hdr.seq);
            break;
        }
        case MP_OTA_BEGIN:
        case MP_OTA_DATA:
        case MP_OTA_END:
        case MP_OTA_ABORT:
            missed = 0;
            node_ota_handle(type, m.data, m.len, s_gw_mac);
            break;
        case MP_LEAVE:
            ESP_LOGW(TAG, "unpaired by gateway");
            esp_now_del_peer(s_gw_mac);
            node_link_forget();
            return;
        default:
            break;
        }
    }
}

static void link_task(void *arg)
{
#if CONFIG_NODE_TEST_NO_JOIN_AFTER_OTA
    if (s_pending_verify) {
        ESP_LOGE(TAG, "NODE_TEST_NO_JOIN_AFTER_OTA: staying silent, should roll back");
        vTaskDelay(portMAX_DELAY);
    }
#endif
    for (;;) {
        ESP_LOGI(TAG, "scanning for gateway%s", s_bound ? "" : " (unbound, gateway must be in pairing mode)");
        scan_until_joined();
        ESP_LOGI(TAG, "joined gateway " MACSTR " on channel %d", MAC2STR(s_gw_mac), s_gw_ch);
        run_joined();
        node_ota_cancel();
    }
}

static void validate_timeout_cb(void *arg)
{
    if (s_pending_verify) {
        ESP_LOGE(TAG, "new firmware failed to reach gateway, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void check_pending_verify(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    const esp_timer_create_args_t args = {.callback = validate_timeout_cb, .name = "validate"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_validate_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_validate_timer, VALIDATE_TIMEOUT_US));
    s_pending_verify = true;
    ESP_LOGW(TAG, "running unverified firmware, must reach gateway within %lld s",
             VALIDATE_TIMEOUT_US / 1000000);
}

#if CONFIG_NODE_LOW_POWER
static void window_start_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xSemaphoreGive(s_window_sem);
}
#endif

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    rx_msg_t m;
    if (len <= 0 || len > ESP_NOW_MAX_DATA_LEN) {
        return;
    }
    memcpy(m.src, info->src_addr, 6);
    m.len = len;
    memcpy(m.data, data, len);
    xQueueSend(s_rx_q, &m, 0);
}

esp_err_t node_link_start(const mp_ch_desc_t desc[MP_MAX_CH], uint8_t n_ch)
{
    s_rx_q = xQueueCreate(16, sizeof(rx_msg_t));   /* 升级时网关一次连发 8 片 */
    if (!s_rx_q) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_desc, desc, sizeof(s_desc));
    s_n_ch = n_ch;

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#if CONFIG_NODE_LOW_POWER
    s_window_sem = xSemaphoreCreateBinary();
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,     /* XTAL */
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_CONNECTIONLESS_MODULE_WAKE_INTERVAL_START,
                                               window_start_cb, NULL));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_NODE_LOW_POWER
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(CONFIG_NODE_LP_INTERVAL_MS));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    esp_wifi_get_mac(WIFI_IF_STA, s_my_mac);
    snprintf(s_name, sizeof(s_name), "%s-%02X%02X", CONFIG_NODE_NAME, s_my_mac[4], s_my_mac[5]);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(mp_espnow_setup());
    ESP_ERROR_CHECK(mp_peer_upsert(MP_BROADCAST, false));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    load_binding();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "node \"%s\" " MACSTR ", %s v%s, %d channels", s_name, MAC2STR(s_my_mac),
             app->project_name, app->version, s_n_ch);
    check_pending_verify();
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        s_stay_until_us = COLD_AWAKE_US;
    }

    /* 升级收尾时 esp_ota_end 要校验签名，4K 栈不够 */
    if (xTaskCreate(link_task, "node_link", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

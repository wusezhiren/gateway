#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "gw_wifi.h"

static const char *TAG = "gw_wifi";

#define NVS_NS          "gw_wifi"
#define RETRY_MIN_MS    5000
#define RETRY_MAX_MS    (5 * 60 * 1000)
#define TRY_TIMEOUT_MS  30000
#define TRY_MAX_ATTEMPT 3

typedef enum { MODE_AUTO, MODE_PROV } wifi_mode_sel_t;

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static TimerHandle_t s_retry_timer;
static TimerHandle_t s_try_timer;
static SemaphoreHandle_t s_lock;

static volatile wifi_mode_sel_t s_mode = MODE_AUTO;
static bool s_configured;
static char s_ssid[33];
static uint8_t s_home_ch;           /* 上次连上路由器时的信道，ESP-NOW 节点就在这个信道上 */
static volatile bool s_connected;
static volatile bool s_associated;
static esp_ip4_addr_t s_ip;
static uint32_t s_retry_ms = RETRY_MIN_MS;

static volatile gw_try_state_t s_try = GW_TRY_IDLE;
static volatile bool s_try_active;  /* 试连的 connect 已发出，断开事件归属试连 */
static int s_try_attempt;
static char s_try_ssid[33];
static char s_try_pass[65];

/* 最近一次扫描结果，试连时用来查目标 AP 的信道 */
static wifi_ap_record_t s_scan_recs[32];
static uint16_t s_scan_n;

/* ---------- NVS ---------- */

static esp_err_t nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "pass", pass ? pass : "");
        }
    } else {
        err = nvs_erase_key(h, "ssid");
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            nvs_erase_key(h, "pass");
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void nvs_save_home_ch(uint8_t ch)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ch", ch);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- 内部 ---------- */

static void go_home_channel(void)
{
    /* STA 未连接时才允许设置；ESP-NOW 节点在这个信道上 */
    esp_wifi_set_channel(s_home_ch, WIFI_SECOND_CHAN_NONE);
}

static void sta_config_set(const char *ssid, const char *pass, uint8_t ch_hint)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    /* 有密码时接受 WPA/WPA2/WPA3，兼容老路由器 */
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.channel = ch_hint;      /* 先扫这个信道，减少跳信道时间 */
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/* 从 NVS 加载凭据并写入 STA 配置 */
static void load_and_apply(void)
{
    char pass[65] = "";
    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(pass);
    nvs_handle_t h;

    s_configured = false;
    s_ssid[0] = '\0';
    s_home_ch = CONFIG_GW_DEFAULT_CHANNEL;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", s_ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK && s_ssid[0] != '\0') {
            s_configured = true;
        }
        uint8_t ch;
        if (s_configured && nvs_get_u8(h, "ch", &ch) == ESP_OK && ch >= 1 && ch <= 13) {
            s_home_ch = ch;
        }
        nvs_close(h);
    }

    if (s_configured) {
        sta_config_set(s_ssid, pass, s_home_ch);
        ESP_LOGI(TAG, "router \"%s\" configured, home channel %d", s_ssid, s_home_ch);
    } else {
        s_ssid[0] = '\0';
        ESP_LOGI(TAG, "no router configured, ESP-NOW on channel %d", s_home_ch);
    }
}

static void auto_connect_now(void)
{
    s_retry_ms = RETRY_MIN_MS;
    xTimerStop(s_retry_timer, 0);
    if (s_configured && !s_connected) {
        esp_wifi_connect();
    } else if (!s_configured) {
        go_home_channel();
    }
}

static void retry_cb(TimerHandle_t t)
{
    if (s_mode == MODE_AUTO && s_configured && !s_connected) {
        esp_wifi_connect();
    }
}

static gw_try_state_t reason_to_try(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
        return GW_TRY_BAD_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return GW_TRY_NO_AP;
    default:
        return GW_TRY_FAILED;
    }
}

static void try_finish(gw_try_state_t result)
{
    s_try_active = false;
    xTimerStop(s_try_timer, 0);
    s_try = result;
}

static void try_timeout_cb(TimerHandle_t t)
{
    if (s_try == GW_TRY_CONNECTING) {
        ESP_LOGW(TAG, "try \"%s\" timed out", s_try_ssid);
        try_finish(s_associated ? GW_TRY_NO_IP : GW_TRY_FAILED);
        esp_wifi_disconnect();
    }
}

static void on_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    s_connected = false;
    s_associated = false;
    s_ip.addr = 0;

    if (s_try_active && s_try == GW_TRY_CONNECTING) {
        gw_try_state_t r = reason_to_try(ev->reason);
        if (++s_try_attempt < TRY_MAX_ATTEMPT) {
            ESP_LOGW(TAG, "try \"%s\" attempt %d failed (reason %d), retry", s_try_ssid, s_try_attempt, ev->reason);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "try \"%s\" failed (reason %d)", s_try_ssid, ev->reason);
            try_finish(r);
        }
        return;
    }

    if (s_mode == MODE_AUTO && s_configured) {
        go_home_channel();
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %lu s", ev->reason, (unsigned long)(s_retry_ms / 1000));
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        xTimerStart(s_retry_timer, 0);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    } else if (!s_configured) {
        go_home_channel();
    }
}

static void on_got_ip(const ip_event_got_ip_t *ev)
{
    s_ip = ev->ip_info.ip;
    s_connected = true;
    s_retry_ms = RETRY_MIN_MS;

    if (s_try_active && s_try == GW_TRY_CONNECTING) {
        if (nvs_save_creds(s_try_ssid, s_try_pass) == ESP_OK) {
            s_configured = true;
            strlcpy(s_ssid, s_try_ssid, sizeof(s_ssid));
        }
        ESP_LOGI(TAG, "try \"%s\" ok", s_try_ssid);
        try_finish(GW_TRY_OK);
    }

    uint8_t ch;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&ch, &second) == ESP_OK && ch != s_home_ch) {
        s_home_ch = ch;
        nvs_save_home_ch(ch);
    }
    ESP_LOGI(TAG, "got ip " IPSTR " on \"%s\", channel %d", IP2STR(&s_ip), s_ssid, s_home_ch);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = data;
        s_associated = true;
        ESP_LOGI(TAG, "associated, channel %d", ev->channel);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        on_disconnected(data);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(data);
    }
}

/* ---------- 对外接口 ---------- */

esp_err_t gw_wifi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "esp-gw");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_MIN_MS), pdFALSE, NULL, retry_cb);
    s_try_timer = xTimerCreate("wifi_try", pdMS_TO_TICKS(TRY_TIMEOUT_MS), pdFALSE, NULL, try_timeout_cb);

    load_and_apply();
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 网关要一直收 ESP-NOW，关闭省电，否则 modem sleep 期间会丢帧 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (!s_configured) {
        go_home_channel();
    }
    return ESP_OK;
}

esp_err_t gw_wifi_set_credentials(const char *ssid, const char *password)
{
    if (s_mode != MODE_AUTO) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = nvs_save_creds(ssid, password);
    if (err == ESP_OK) {
        /* 先标记未配置，避免旧连接的断开事件触发重连 */
        s_configured = false;
        xTimerStop(s_retry_timer, 0);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        load_and_apply();
        auto_connect_now();
    }
    xSemaphoreGive(s_lock);
    return err;
}

void gw_wifi_get_status(gw_wifi_status_t *st)
{
    wifi_second_chan_t second;
    memset(st, 0, sizeof(*st));
    st->configured = s_configured;
    st->connected = s_connected;
    st->prov = s_mode == MODE_PROV;
    strlcpy(st->ssid, s_ssid, sizeof(st->ssid));
    esp_wifi_get_channel(&st->channel, &second);
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
        esp_ip4_addr_t ip = s_ip;
        snprintf(st->ip, sizeof(st->ip), IPSTR, IP2STR(&ip));
    }
}

esp_err_t gw_wifi_prov_begin(const char *ap_ssid, const char *ap_password)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = MODE_PROV;
    s_try = GW_TRY_IDLE;
    xTimerStop(s_retry_timer, 0);

    uint8_t ch;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&ch, &second);
    if (!s_connected) {
        /* 自动重连可能正扫到别的信道，热点放回 ESP-NOW 信道 */
        esp_wifi_disconnect();
        ch = s_home_ch;
    }

    wifi_config_t ap = {
        .ap = {
            .channel = ch,
            .max_connection = 3,    /* 与 ESP-NOW 加密 peer 共用硬件密钥，见 sdkconfig.defaults */
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, ap_password, sizeof(ap.ap.password));

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (err == ESP_OK && !s_connected) {
        go_home_channel();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "softap \"%s\" up on channel %d: %s", ap_ssid, ch, esp_err_to_name(err));
    return err;
}

void gw_wifi_prov_end(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xTimerStop(s_try_timer, 0);
    s_try_active = false;
    s_try = GW_TRY_IDLE;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_mode = MODE_AUTO;
    if (!s_connected) {
        /* 试连失败或取消：恢复 NVS 里的原凭据 */
        esp_wifi_disconnect();
        load_and_apply();
    }
    auto_connect_now();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "softap down");
}

int gw_wifi_scan(wifi_ap_record_t *out, int max)
{
    if (s_try == GW_TRY_CONNECTING) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wifi_scan_config_t cfg = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    uint16_t n = 0;
    wifi_ap_record_t *recs = s_scan_recs;
    if (err == ESP_OK) {
        n = sizeof(s_scan_recs) / sizeof(s_scan_recs[0]);
        esp_wifi_scan_get_ap_records(&n, recs);
        s_scan_n = n;
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    /* 扫描结束后驱动会回到原信道（APSTA 下即热点信道），不需要手动切 */
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return -1;
    }

    /* 结果已按 RSSI 降序，同名只保留最强的 */
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < cnt; j++) {
            if (strcmp((const char *)out[j].ssid, (const char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out[cnt++] = recs[i];
        }
    }
    return cnt;
}

esp_err_t gw_wifi_try_connect(const char *ssid, const char *password)
{
    if (s_mode != MODE_PROV) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
    strlcpy(s_try_pass, password ? password : "", sizeof(s_try_pass));
    s_try = GW_TRY_CONNECTING;
    s_try_active = false;
    s_try_attempt = 0;

    /* 断开现有连接；PROV 模式下不会触发自动重连，等断开事件处理完再发起试连 */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    /* 扫描里见过就先扫那个信道：热点离开本信道的时间越短，手机越不容易断 */
    uint8_t hint = 0;
    for (int i = 0; i < s_scan_n; i++) {
        if (strcmp((const char *)s_scan_recs[i].ssid, s_try_ssid) == 0) {
            hint = s_scan_recs[i].primary;
            break;
        }
    }
    sta_config_set(s_try_ssid, s_try_pass, hint);
    s_try_active = true;
    xTimerStart(s_try_timer, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        try_finish(GW_TRY_FAILED);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "trying \"%s\" (channel hint %d): %s", s_try_ssid, hint, esp_err_to_name(err));
    return err;
}

gw_try_state_t gw_wifi_try_state(void)
{
    return s_try;
}

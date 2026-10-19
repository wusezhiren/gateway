#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "gw_wifi.h"

static const char *TAG = "gw_wifi";

#define NVS_NS          "gw_wifi"
#define RETRY_DELAY_MS  5000

static esp_netif_t *s_netif;
static TimerHandle_t s_retry_timer;
static bool s_configured;
static volatile bool s_connected;
static char s_ssid[33];
static esp_ip4_addr_t s_ip;

static void apply_default_channel(void)
{
    /* STA 断开是异步的，还没断开时会失败，断开事件里会再调一次 */
    if (esp_wifi_set_channel(CONFIG_GW_DEFAULT_CHANNEL, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        ESP_LOGI(TAG, "no router configured, staying on channel %d", CONFIG_GW_DEFAULT_CHANNEL);
    }
}

static void retry_cb(TimerHandle_t t)
{
    if (s_configured && !s_connected) {
        esp_wifi_connect();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = data;
        ESP_LOGI(TAG, "associated to \"%s\", channel %d", s_ssid, ev->channel);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_connected = false;
        s_ip.addr = 0;
        if (s_configured) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry in %d ms", ev->reason, RETRY_DELAY_MS);
            xTimerStart(s_retry_timer, 0);
        } else {
            apply_default_channel();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        s_ip = ev->ip_info.ip;
        s_connected = true;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&s_ip));
    }
}

static void load_and_apply(void)
{
    wifi_config_t cfg = {0};
    size_t ssid_len = sizeof(cfg.sta.ssid);
    size_t pass_len = sizeof(cfg.sta.password);
    nvs_handle_t h;

    s_configured = false;
    s_ssid[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", (char *)cfg.sta.ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(h, "pass", (char *)cfg.sta.password, &pass_len) == ESP_OK &&
            cfg.sta.ssid[0] != '\0') {
            s_configured = true;
            strlcpy(s_ssid, (const char *)cfg.sta.ssid, sizeof(s_ssid));
        }
        nvs_close(h);
    }

    if (s_configured) {
        /* 开放网络时允许 OPEN，否则至少 WPA2 */
        cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        ESP_LOGI(TAG, "router \"%s\" configured", s_ssid);
    } else {
        apply_default_channel();
    }
}

esp_err_t gw_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "esp-gw");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_DELAY_MS), pdFALSE, NULL, retry_cb);

    ESP_ERROR_CHECK(esp_wifi_start());
    /* 网关要一直收 ESP-NOW，关闭省电，否则 modem sleep 期间会丢帧 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    load_and_apply();
    if (s_configured) {
        esp_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t gw_wifi_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "pass", password ? password : "");
        }
    } else {
        err = nvs_erase_all(h);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    /* 先标记未配置，避免 disconnect 事件触发重连旧网络 */
    s_configured = false;
    xTimerStop(s_retry_timer, 0);
    esp_wifi_disconnect();
    load_and_apply();
    if (s_configured) {
        esp_wifi_connect();
    }
    return ESP_OK;
}

void gw_wifi_get_status(gw_wifi_status_t *st)
{
    wifi_second_chan_t second;
    memset(st, 0, sizeof(*st));
    st->configured = s_configured;
    st->connected = s_connected;
    strlcpy(st->ssid, s_ssid, sizeof(st->ssid));
    esp_wifi_get_channel(&st->channel, &second);
    if (s_connected) {
        esp_ip4_addr_t ip = s_ip;
        snprintf(st->ip, sizeof(st->ip), IPSTR, IP2STR(&ip));
    }
}

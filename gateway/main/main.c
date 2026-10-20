#include <stdlib.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "gw_api.h"
#include "gw_cloud.h"
#include "gw_display.h"
#include "gw_http.h"
#include "gw_keys.h"
#include "gw_link.h"
#include "gw_ota.h"
#include "gw_prov.h"
#include "gw_wifi.h"
#include "ui.h"

void gw_console_start(void);

static const char *TAG = "gateway";

static void on_event(gw_event_t ev, int idx)
{
    static const char *const names[] = {
        [GW_EV_NODE_JOINED] = "joined",
        [GW_EV_NODE_OFFLINE] = "offline",
        [GW_EV_NODE_STATE] = "state",
        [GW_EV_NODE_REMOVED] = "removed",
        [GW_EV_SET_FAILED] = "set_failed",
    };
    ESP_LOGI(TAG, "event %s node %d", names[ev], idx);
    gw_cloud_notify();
}

static bool s_ui_ok;

static void on_key(gw_key_t key, gw_key_event_t ev)
{
    if (s_ui_ok) {
        ui_on_key(key, ev);
    }
    if (key == GW_KEY_REC && ev == GW_KEY_EV_LONG) {
        if (gw_prov_on_long_press() == GW_PROV_KEY_NEED_CONFIRM && s_ui_ok) {
            ui_request_prov_confirm();
        }
    }
}

static void on_prov(const gw_prov_status_t *st)
{
    ESP_LOGI(TAG, "provisioning %s", gw_prov_state_name(st->state));
    gw_cloud_notify();
}

static void on_ota(const gw_ota_status_t *st)
{
    ESP_LOGI(TAG, "ota %s v%s %lu/%lu %s", gw_ota_state_name(st->state), st->version,
             (unsigned long)st->received, (unsigned long)st->total, st->error);
    gw_cloud_notify();
}

#define VALIDATE_TIMEOUT_S 120

/*
 * OTA 后首次启动处于 PENDING_VERIFY：ESP-NOW 已启动，且 Wi-Fi 连上（或未配置路由器）即确认有效。
 * 路由器故障不应导致回滚，所以超时后也确认；新固件若在确认前崩溃重启，bootloader 会自动回滚。
 */
static void validate_task(void *arg)
{
#if CONFIG_GW_TEST_ABORT_BEFORE_VALIDATE
    ESP_LOGE(TAG, "GW_TEST_ABORT_BEFORE_VALIDATE: aborting, bootloader should roll back");
    vTaskDelay(pdMS_TO_TICKS(2000));
    abort();
#endif
    for (int i = 0; i < VALIDATE_TIMEOUT_S; i++) {
        gw_wifi_status_t st;
        gw_wifi_get_status(&st);
        if (!st.configured || st.connected) {
            break;
        }
        if (i == VALIDATE_TIMEOUT_S - 1) {
            ESP_LOGW(TAG, "router not reachable, accept new firmware anyway");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    ESP_LOGI(TAG, "new firmware marked valid");
    vTaskDelete(NULL);
}

static bool pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

/* App 通过 mDNS 发现网关：esp-gw.local，服务 _espgw._tcp */
static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mdns init failed");
        return;
    }
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char inst[24], mac_str[18];
    snprintf(inst, sizeof(inst), "ESP-GW-%02X%02X", mac[4], mac[5]);
    snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(mac));
    mdns_hostname_set("esp-gw");
    mdns_instance_name_set(inst);
    mdns_txt_item_t txt[] = {
        {"mac", mac_str},
        {"version", esp_app_get_description()->version},
    };
    mdns_service_add(inst, "_espgw", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
    mdns_service_add(inst, "_http", "_tcp", 80, NULL, 0);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s (%s)", app->project_name, app->version,
             esp_ota_get_running_partition()->label);

    ESP_ERROR_CHECK(gw_wifi_init());
    mdns_start();

    /* 联网后自动对时，状态栏显示时间用 */
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&sntp);
    gw_link_set_event_cb(on_event);
    ESP_ERROR_CHECK(gw_link_start());
    ESP_ERROR_CHECK(gw_cloud_init());
    ESP_ERROR_CHECK(gw_http_start());
    gw_ota_set_cb(on_ota);
    ESP_ERROR_CHECK(gw_prov_init(on_prov));
    /* 屏幕失败不影响组网功能 */
    if (gw_display_init() == ESP_OK) {
        ui_start();
        s_ui_ok = true;
        gw_api_set_bind_cb(ui_request_bind_confirm);
    } else {
        ESP_LOGE(TAG, "display init failed, running headless");
    }
    ESP_ERROR_CHECK(gw_keys_start(on_key));

    gw_wifi_status_t ws;
    gw_wifi_get_status(&ws);
    if (!ws.configured) {
        /* 从未配置过路由器：开机直接进入配网 */
        gw_prov_start();
    }

    if (pending_verify()) {
        xTaskCreate(validate_task, "validate", 3072, NULL, 3, NULL);
    }
    gw_console_start();
}

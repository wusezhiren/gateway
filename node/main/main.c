#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "node_io.h"
#include "node_link.h"

static const char *TAG = "node";

#define RESET_HOLD_MS 3000

/* 长按复位键清除网关绑定并重启 */
static void reset_button_task(void *arg)
{
    const int gpio = CONFIG_NODE_RESET_GPIO;
    int held_ms = 0;

    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        held_ms = gpio_get_level(gpio) == 0 ? held_ms + 50 : 0;
        if (held_ms >= RESET_HOLD_MS) {
            ESP_LOGW(TAG, "reset button held, forgetting gateway");
            node_link_forget();
            esp_restart();
        }
    }
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

    mp_ch_desc_t desc[MP_MAX_CH];
    uint8_t n_ch = 0;
    ESP_ERROR_CHECK(node_io_init(desc, &n_ch));
    ESP_ERROR_CHECK(node_link_start(desc, n_ch));

    if (CONFIG_NODE_RESET_GPIO >= 0) {
        xTaskCreate(reset_button_task, "reset_btn", 2048, NULL, 3, NULL);
    }
}

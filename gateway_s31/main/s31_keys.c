#include "s31_keys.h"

#include <stdlib.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "s31_keys";
static s31_key_callback_t s_callback;
static void *s_context;

static void keys_task(void *arg)
{
    int last = -1;
    (void)arg;
    for (;;) {
        int raw = bsp_adc_read_raw();
        int mv = bsp_adc_read_mv();
        int key = mv < 600 ? 0 : mv < 1080 ? 1 : mv < 1605 ? 2 : mv < 2200 ? 3 : -1;
        if (key != last) {
            ESP_LOGI(TAG, "key=%d raw=%d mv=%d", key, raw, mv);
            if (s_callback) s_callback(key, key >= 0, s_context);
            last = key;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

esp_err_t s31_keys_start(s31_key_callback_t callback, void *context)
{
    s_callback = callback;
    s_context = context;
    return xTaskCreate(keys_task, "s31_keys", 3072, NULL, 4, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

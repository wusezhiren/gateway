#include "esp_log.h"
#include "board_s31.h"
#include "s31_msgbus.h"
#include "voice_core/voice_core.h"

static const char *TAG = "gateway_s31";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S31 gateway board bring-up");
    ESP_LOGI(TAG, "voice core ready for simplex integration (idle=%d)", VOICE_STATE_IDLE);
    ESP_ERROR_CHECK(s31_msgbus_init());
    esp_err_t err = gw_s31_board_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card unavailable: %s", esp_err_to_name(err));
    }
}

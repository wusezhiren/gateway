#include "board_s31.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_GW_S31_ENABLE_DISPLAY
#include "s31_display.h"
#endif
#if CONFIG_GW_S31_ENABLE_AUDIO
#include "s31_audio.h"
#endif
#if CONFIG_GW_S31_ENABLE_SD
#include "gw_sd_s31.h"
#endif
#if CONFIG_GW_S31_ENABLE_KEYS
#include "s31_keys.h"
#endif
#if CONFIG_GW_S31_ENABLE_WS2812
#include "s31_led.h"
#endif
#if CONFIG_GW_S31_ENABLE_BUZZER
#include "s31_buzzer.h"
#endif

static const char *TAG = "s31_board";

esp_err_t gw_s31_board_init(void)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t err __attribute__((unused));

#if CONFIG_GW_S31_ENABLE_DISPLAY
    err = s31_display_init();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
#endif
#if CONFIG_GW_S31_ENABLE_AUDIO
    err = s31_audio_init();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
#endif
#if CONFIG_GW_S31_ENABLE_SD
    err = gw_sd_s31_init();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
#endif
#if CONFIG_GW_S31_ENABLE_KEYS
    err = s31_keys_start(NULL, NULL);
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
#endif
#if CONFIG_GW_S31_ENABLE_WS2812
    err = s31_led_init();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
#endif
#if CONFIG_GW_S31_ENABLE_BUZZER
    err = s31_buzzer_init();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
#endif

    ESP_LOGI(TAG, "board peripherals initialized: %s", esp_err_to_name(first_error));
    return first_error;
}

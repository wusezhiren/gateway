#include "s31_led.h"

#include "led_strip.h"

static led_strip_handle_t s_led;

esp_err_t s31_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = 37,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {.resolution_hz = 10 * 1000 * 1000};
    return led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led);
}

esp_err_t s31_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return ESP_ERR_INVALID_STATE;
    esp_err_t err = led_strip_set_pixel(s_led, 0, r, g, b);
    if (err == ESP_OK) err = led_strip_refresh(s_led);
    return err;
}

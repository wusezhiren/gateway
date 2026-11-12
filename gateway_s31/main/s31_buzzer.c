#include "s31_buzzer.h"

#include "driver/gpio.h"

esp_err_t s31_buzzer_init(void)
{
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << 46, .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_set_level(GPIO_NUM_46, 0);
    return err;
}

esp_err_t s31_buzzer_set(bool on)
{
    return gpio_set_level(GPIO_NUM_46, on ? 1 : 0);
}

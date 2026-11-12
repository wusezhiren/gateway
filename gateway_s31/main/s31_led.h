#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t s31_led_init(void);
esp_err_t s31_led_set(uint8_t r, uint8_t g, uint8_t b);

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t s31_buzzer_init(void);
esp_err_t s31_buzzer_set(bool on);

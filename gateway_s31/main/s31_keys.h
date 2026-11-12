#pragma once

#include "esp_err.h"

typedef void (*s31_key_callback_t)(int key, bool pressed, void *context);
esp_err_t s31_keys_start(s31_key_callback_t callback, void *context);

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t s31_audio_init(void);
esp_err_t s31_audio_capture(int16_t *samples, size_t sample_count, size_t *read_samples);
esp_err_t s31_audio_play(const int16_t *samples, size_t sample_count);

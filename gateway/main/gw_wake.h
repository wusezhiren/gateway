#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

esp_err_t gw_wake_init(void);
esp_err_t gw_wake_start(void);
void gw_wake_pause(void);
void gw_wake_resume(void);
esp_err_t gw_wake_suspend_for_voice(void);
esp_err_t gw_wake_resume_after_voice(void);
size_t gw_wake_take_handoff(int16_t *out, size_t capacity);
esp_err_t gw_wake_begin_barge_in(void);
bool gw_wake_barge_in_detected(void);
bool gw_wake_take_barge_in(void);
void gw_wake_discard_barge_in(void);
void gw_wake_end_barge_in(void);
bool gw_wake_barge_in_ready(void);
void gw_wake_barge_in_channels_ready(void);
void gw_wake_status(void);
esp_err_t gw_wake_test_pcm(const uint8_t *pcm, size_t bytes);

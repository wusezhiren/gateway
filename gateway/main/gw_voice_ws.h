#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t gw_voice_ws_init(void);
esp_err_t gw_voice_ws_config(const char *url, const char *token);
esp_err_t gw_voice_ws_activate(bool confirm);
esp_err_t gw_voice_ws_start(void);
esp_err_t gw_voice_ws_listen(void);
esp_err_t gw_voice_ws_listen_with_preroll(const int16_t *pcm, size_t samples);
esp_err_t gw_voice_ws_listen_auto(void);
esp_err_t gw_voice_ws_listen_pcm(const int16_t *pcm, size_t samples);
esp_err_t gw_voice_ws_listen_pcm_auto(const int16_t *pcm, size_t samples);
esp_err_t gw_voice_ws_probe_barge_pcm(const int16_t *pcm, size_t samples);
void gw_voice_ws_stop(void);
void gw_voice_ws_status(void);

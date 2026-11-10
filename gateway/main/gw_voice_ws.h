#pragma once

#include "esp_err.h"

esp_err_t gw_voice_ws_init(void);
esp_err_t gw_voice_ws_config(const char *url, const char *token);
esp_err_t gw_voice_ws_start(void);
void gw_voice_ws_stop(void);
void gw_voice_ws_status(void);

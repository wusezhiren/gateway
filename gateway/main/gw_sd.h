#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t gw_sd_init(void);
bool gw_sd_is_mounted(void);
void gw_sd_status(void);
esp_err_t gw_sd_upload_file(const char *name, const uint8_t *data, size_t len);
esp_err_t gw_sd_play_wake_reply(void);

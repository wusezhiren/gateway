#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 Wi-Fi STA；NVS 里有路由器凭据就去连，没有则停在 CONFIG_GW_DEFAULT_CHANNEL */
esp_err_t gw_wifi_init(void);

/* 保存凭据并重新连接；ssid 为 NULL 表示清除凭据 */
esp_err_t gw_wifi_set_credentials(const char *ssid, const char *password);

typedef struct {
    bool configured;
    bool connected;
    char ssid[33];
    uint8_t channel;
    char ip[16];
} gw_wifi_status_t;

void gw_wifi_get_status(gw_wifi_status_t *st);

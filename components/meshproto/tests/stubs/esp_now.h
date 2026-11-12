#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_NOW_KEY_LEN 16
#define ESP_NOW_MAX_DATA_LEN 250
typedef struct {
    uint8_t peer_addr[6];
    uint8_t lmk[ESP_NOW_KEY_LEN];
    int channel;
    int ifidx;
    bool encrypt;
} esp_now_peer_info_t;
bool esp_now_is_peer_exist(const uint8_t *mac);
esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer);
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer);
esp_err_t esp_now_set_pmk(const uint8_t *pmk);
#define WIFI_IF_STA 0

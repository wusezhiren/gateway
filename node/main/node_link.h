#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "meshproto.h"

/* 初始化 Wi-Fi/ESP-NOW 并启动链路任务，需先初始化 NVS 和默认事件循环 */
esp_err_t node_link_start(const mp_ch_desc_t desc[MP_MAX_CH], uint8_t n_ch);

/* 清除网关绑定（NVS），重启后需要网关处于配对模式才能重新入网 */
void node_link_forget(void);

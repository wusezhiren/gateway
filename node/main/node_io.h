#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "meshproto.h"

/* 按 Kconfig 初始化执行器，填写通道描述（继电器在前，PWM 在后） */
esp_err_t node_io_init(mp_ch_desc_t desc[MP_MAX_CH], uint8_t *n_ch);

esp_err_t node_io_set(uint8_t ch, uint16_t value);

/* 读出全部通道当前值 */
void node_io_get(uint16_t value[MP_MAX_CH]);

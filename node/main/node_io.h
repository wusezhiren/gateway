#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "meshproto.h"

/* 按 Kconfig 初始化执行器，填写通道描述（继电器在前，然后 PWM、呼吸灯、慢闪，彩灯在最后） */
esp_err_t node_io_init(mp_ch_desc_t desc[MP_MAX_CH], uint8_t *n_ch);

esp_err_t node_io_set(uint8_t ch, uint16_t value);

/* 读出全部通道当前值 */
void node_io_get(uint16_t value[MP_MAX_CH]);

/* 全部通道都关着（彩灯看开关位） */
bool node_io_all_off(void);

/* 深睡前调用：落盘未保存的状态，保持输出引脚电平 */
void node_io_prepare_sleep(void);

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* KS_S3_KORVO_V4 的 6 个 ADC 分压按键（ADC1_CH4 / GPIO5），序号按分压从低到高 */
typedef enum {
    GW_KEY_VOL_UP = 0,
    GW_KEY_VOL_DOWN,
    GW_KEY_2,
    GW_KEY_3,
    GW_KEY_4,
    GW_KEY_REC,         /* 配网键 */
    GW_KEY_COUNT,
} gw_key_t;

typedef enum {
    GW_KEY_EV_PRESS,    /* 按下（防抖后） */
    GW_KEY_EV_CLICK,    /* 短按松开 */
    GW_KEY_EV_LONG,     /* 按住满 GW_KEY_LONG_MS，每次按下只触发一次 */
} gw_key_event_t;

#define GW_KEY_LONG_MS 3000

/* 回调在按键任务里执行，不要阻塞 */
typedef void (*gw_key_cb_t)(gw_key_t key, gw_key_event_t ev);

esp_err_t gw_keys_start(gw_key_cb_t cb);

/* 调试：打印 ADC 读数变化，用于实物确认按键分压 */
void gw_keys_set_debug(bool on);

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/*
 * KS_S3_KORVO_V4 屏幕：ST7789 280x240（SPI），CST816 触摸（I2C1），TCA9554 控制屏幕电源/背光。
 * 初始化后 LVGL 由 esp_lvgl_port 的任务驱动；其他任务操作 LVGL 前必须 gw_display_lock()。
 */

#define GW_LCD_W 280
#define GW_LCD_H 240

esp_err_t gw_display_init(void);
lv_display_t *gw_display_get(void);
lv_indev_t *gw_display_touch(void);    /* 触摸不可用时为 NULL */

bool gw_display_lock(int timeout_ms);
void gw_display_unlock(void);

/* 熄屏/亮屏：面板休眠 + 关背光（背光引脚见 CONFIG_GW_LCD_BL_EXP_PIN） */
void gw_display_set_on(bool on);

/* 调试：直接设置 TCA9554 某个引脚电平，用于确认哪一脚是背光 */
esp_err_t gw_display_exp_set(int pin, int level);

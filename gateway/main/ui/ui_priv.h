#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "gw_cloud.h"
#include "gw_link.h"
#include "gw_ota.h"
#include "gw_prov.h"
#include "gw_wifi.h"

LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_24);

#define UI_W        280
#define UI_H        240
#define UI_BAR_H    26
#define UI_BODY_H   (UI_H - UI_BAR_H)

#define UI_C_BG     lv_color_hex(0x0f1216)
#define UI_C_CARD   lv_color_hex(0x1b2027)
#define UI_C_LINE   lv_color_hex(0x2a313b)
#define UI_C_TEXT   lv_color_hex(0xe8eaed)
#define UI_C_SUB    lv_color_hex(0x8b939e)
#define UI_C_ACCENT lv_color_hex(0x3c8cff)
#define UI_C_OK     lv_color_hex(0x2bb673)
#define UI_C_WARN   lv_color_hex(0xf0a020)
#define UI_C_ERR    lv_color_hex(0xe5484d)

/* 每 500ms 在 LVGL 任务里刷新的数据快照，各页面只读它 */
typedef struct {
    gw_wifi_status_t wifi;
    gw_prov_status_t prov;
    gw_ota_status_t ota;
    gw_cloud_status_t cloud;
    gw_node_t nodes[GW_MAX_NODES];
    int node_total;
    int node_online;
    uint32_t pair_left;
    char fw_version[32];
    char partition[16];
} ui_model_t;

extern ui_model_t g_ui;

/* 公共控件 */
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user);
void ui_set_hidden(lv_obj_t *obj, bool hidden);

typedef void (*ui_confirm_cb_t)(void *arg);
void ui_confirm(const char *title, const char *msg, const char *ok_text, lv_color_t ok_color,
                ui_confirm_cb_t on_ok, void *arg);
/* on_cancel 在点取消或被新对话框顶掉时调用 */
void ui_confirm_ex(const char *title, const char *msg, const char *ok_text, lv_color_t ok_color,
                   ui_confirm_cb_t on_ok, ui_confirm_cb_t on_cancel, void *arg);
bool ui_dialog_open(void);
void ui_toast(const char *msg);

const char *ui_rssi_text(int8_t rssi);

/* 页面 */
void ui_overview_create(lv_obj_t *tile);
void ui_overview_update(void);
void ui_nodes_create(lv_obj_t *tile);
void ui_nodes_update(void);
void ui_node_detail_update(void);
void ui_fw_create(lv_obj_t *tile);
void ui_fw_update(void);
void ui_settings_create(lv_obj_t *tile);
void ui_settings_update(void);
void ui_settings_start_prov(void);

/* 全屏层：配网、网关 OTA；返回是否有全屏层显示（显示时不熄屏） */
void ui_overlay_create(void);
bool ui_overlay_update(void);

/* 熄屏时间（秒，0 常亮），设置页修改后保存 */
uint32_t ui_screen_off_s(void);
void ui_set_screen_off_s(uint32_t s);

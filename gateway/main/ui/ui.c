#include <stdarg.h>
/*
 * UI 框架：状态栏 + 横向 tileview（概览 / 节点 / 固件 / 设置）+ 页码点，
 * 对话框、提示条、熄屏，以及 500ms 一次的数据快照刷新。所有 LVGL 调用都在 LVGL 任务里。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "gw_display.h"
#include "ui.h"
#include "ui_priv.h"
#include "gw_api.h"

static const char *TAG = "ui";

#define REFRESH_MS  500
#define PAGE_COUNT  4
#define NVS_NS      "gw_ui"

ui_model_t g_ui;

static lv_obj_t *s_tv;
static lv_obj_t *s_tiles[PAGE_COUNT];
static lv_obj_t *s_dots[PAGE_COUNT];
static lv_obj_t *s_bar_wifi;
static lv_obj_t *s_bar_title;
static lv_obj_t *s_bar_right;
static lv_obj_t *s_dialog;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_blocker;        /* 熄屏时盖在最上层，吞掉唤醒的那次触摸 */

static bool s_screen_on = true;
static uint32_t s_screen_off_s = CONFIG_GW_SCREEN_OFF_S;

/* 其他任务写、LVGL 任务读的请求标志 */
static volatile int s_key_page_delta;
static volatile bool s_key_wake;
static volatile bool s_req_prov_confirm;
static volatile bool s_req_bind;

static const char *const PAGE_TITLES[PAGE_COUNT] = {"概览", "节点", "固件", "设置"};

/* ---------- 公共控件 ---------- */

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, UI_C_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    ui_label_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_t *l = ui_label(b, &font_cn_16, lv_color_white(), text);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

void ui_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_label_fmt(lv_obj_t *label, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_label_text(label, buf);
}

void ui_text_color(lv_obj_t *obj, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(obj, c, 0);
    }
}

void ui_bg_color(lv_obj_t *obj, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c)) {
        lv_obj_set_style_bg_color(obj, c, 0);
    }
}

void ui_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

const char *ui_rssi_text(int8_t rssi)
{
    if (rssi == 0) {
        return "-";
    }
    return rssi >= -55 ? "强" : rssi >= -70 ? "中" : "弱";
}

/* ---------- 对话框 ---------- */

typedef struct {
    ui_confirm_cb_t cb;
    ui_confirm_cb_t cancel;
    void *arg;
} confirm_ctx_t;

static confirm_ctx_t s_confirm;

static void dialog_close(void)
{
    if (s_dialog) {
        lv_obj_delete(s_dialog);
        s_dialog = NULL;
    }
}

static void dialog_ok_cb(lv_event_t *e)
{
    confirm_ctx_t ctx = s_confirm;
    dialog_close();
    if (ctx.cb) {
        ctx.cb(ctx.arg);
    }
}

static void dialog_cancel_cb(lv_event_t *e)
{
    confirm_ctx_t ctx = s_confirm;
    dialog_close();
    if (ctx.cancel) {
        ctx.cancel(ctx.arg);
    }
}

void ui_confirm(const char *title, const char *msg, const char *ok_text, lv_color_t ok_color,
                ui_confirm_cb_t on_ok, void *arg)
{
    ui_confirm_ex(title, msg, ok_text, ok_color, on_ok, NULL, arg);
}

void ui_confirm_ex(const char *title, const char *msg, const char *ok_text, lv_color_t ok_color,
                   ui_confirm_cb_t on_ok, ui_confirm_cb_t on_cancel, void *arg)
{
    /* 被新对话框顶掉的旧对话框按取消处理 */
    confirm_ctx_t old = s_confirm;
    bool had = s_dialog != NULL;
    dialog_close();
    if (had && old.cancel) {
        old.cancel(old.arg);
    }
    s_confirm = (confirm_ctx_t){.cb = on_ok, .cancel = on_cancel, .arg = arg};

    /* 半透明遮罩挡住下层点击 */
    s_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dialog, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_dialog, 0, 0);
    lv_obj_set_style_radius(s_dialog, 0, 0);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = ui_card(s_dialog);
    lv_obj_set_width(box, 240);
    lv_obj_center(box);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_style_pad_row(box, 10, 0);
    ui_label(box, &font_cn_24, UI_C_TEXT, title);
    lv_obj_t *m = ui_label(box, &font_cn_16, UI_C_SUB, msg);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    ui_button(row, "取消", UI_C_LINE, dialog_cancel_cb, NULL);
    ui_button(row, ok_text, ok_color, dialog_ok_cb, NULL);
}

bool ui_dialog_open(void)
{
    return s_dialog != NULL;
}

static void toast_timer_cb(lv_timer_t *t)
{
    ui_set_hidden(s_toast, true);
    s_toast_timer = NULL;
}

void ui_toast(const char *msg)
{
    ui_label_text(lv_obj_get_child(s_toast, 0), msg);
    ui_set_hidden(s_toast, false);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) {
        lv_timer_reset(s_toast_timer);
    } else {
        s_toast_timer = lv_timer_create(toast_timer_cb, 2500, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
}

/* ---------- 熄屏 ---------- */

uint32_t ui_screen_off_s(void)
{
    return s_screen_off_s;
}

void ui_set_screen_off_s(uint32_t s)
{
    s_screen_off_s = s;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "off_s", s);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void screen_set(bool on)
{
    if (on == s_screen_on) {
        return;
    }
    s_screen_on = on;
    ui_set_hidden(s_blocker, on);
    if (!on) {
        lv_obj_move_foreground(s_blocker);
    }
    gw_display_set_on(on);
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
}

static void blocker_cb(lv_event_t *e)
{
    lv_display_trigger_activity(NULL);
    screen_set(true);
    lv_indev_wait_release(lv_indev_active());
}

/* ---------- 页面切换 ---------- */

static int active_page(void)
{
    lv_obj_t *t = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_tiles[i] == t) {
            return i;
        }
    }
    return 0;
}

static void update_page_marks(void)
{
    int p = active_page();
    ui_label_text(s_bar_title, PAGE_TITLES[p]);
    for (int i = 0; i < PAGE_COUNT; i++) {
        ui_bg_color(s_dots[i], i == p ? UI_C_ACCENT : UI_C_LINE);
    }
}

static void tile_changed_cb(lv_event_t *e)
{
    update_page_marks();
}

/* ---------- 刷新 ---------- */

static void refresh_model(void)
{
    gw_wifi_get_status(&g_ui.wifi);
    gw_prov_get_status(&g_ui.prov);
    gw_ota_get_status(&g_ui.ota);
    gw_cloud_get_status(&g_ui.cloud);
    gw_link_snapshot(g_ui.nodes);
    g_ui.node_total = g_ui.node_online = 0;
    for (int i = 0; i < GW_MAX_NODES; i++) {
        g_ui.node_total += g_ui.nodes[i].used;
        g_ui.node_online += g_ui.nodes[i].used && g_ui.nodes[i].online;
    }
    g_ui.pair_left = gw_link_pair_remaining();
}

static void update_status_bar(void)
{
    const gw_wifi_status_t *w = &g_ui.wifi;
    if (w->connected) {
        ui_label_fmt(s_bar_wifi, LV_SYMBOL_WIFI " %s", w->ssid);
        ui_text_color(s_bar_wifi, UI_C_OK);
    } else if (w->prov) {
        ui_label_text(s_bar_wifi, LV_SYMBOL_WIFI " 配网中");
        ui_text_color(s_bar_wifi, UI_C_ACCENT);
    } else {
        ui_label_text(s_bar_wifi, LV_SYMBOL_WIFI " 未联网");
        ui_text_color(s_bar_wifi, w->configured ? UI_C_WARN : UI_C_SUB);
    }

    char clock[8] = "";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) {
        strftime(clock, sizeof(clock), "%H:%M", &tm);
    }
    ui_label_fmt(s_bar_right, "%d/%d  %s", g_ui.node_online, g_ui.node_total, clock);
}

static void bind_ok_cb(void *arg)
{
    gw_api_bind_decide(true);
    ui_toast("已允许绑定");
}

static void bind_cancel_cb(void *arg)
{
    gw_api_bind_decide(false);
}

static void handle_requests(void)
{
    int delta = s_key_page_delta;
    s_key_page_delta = 0;
    if (s_key_wake) {
        s_key_wake = false;
        lv_display_trigger_activity(NULL);
        if (!s_screen_on) {
            screen_set(true);   /* 熄屏时按键只亮屏 */
            delta = 0;
        }
    }

    bool keep_on = ui_overlay_update() || ui_dialog_open();
    if (keep_on && !s_screen_on) {
        /* 配网/升级开始时自动亮屏 */
        lv_display_trigger_activity(NULL);
        screen_set(true);
    }
    if (!s_screen_on) {
        return;
    }
    if (!keep_on && s_screen_off_s && lv_display_get_inactive_time(NULL) > s_screen_off_s * 1000) {
        screen_set(false);
        return;
    }

    if (delta && !ui_dialog_open()) {
        int p = active_page() + delta;
        p = p < 0 ? 0 : p >= PAGE_COUNT ? PAGE_COUNT - 1 : p;
        lv_tileview_set_tile_by_index(s_tv, p, 0, LV_ANIM_ON);
        update_page_marks();
    }
    if (s_req_prov_confirm) {
        s_req_prov_confirm = false;
        lv_display_trigger_activity(NULL);
        ui_settings_start_prov();
    }
    if (s_req_bind) {
        s_req_bind = false;
        char name[GW_API_NAME_LEN + 1];
        if (gw_api_bind_pending(name)) {
            lv_display_trigger_activity(NULL);
            char msg[160];
            snprintf(msg, sizeof(msg), "手机“%s”请求绑定此网关，允许后可在 App 中控制所有节点", name);
            ui_confirm_ex("绑定手机", msg, "允许", UI_C_ACCENT, bind_ok_cb, bind_cancel_cb, NULL);
        }
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    refresh_model();
    handle_requests();
    if (!s_screen_on) {
        return;
    }
    update_status_bar();
    ui_overview_update();
    ui_nodes_update();
    ui_node_detail_update();
    ui_fw_update();
    ui_settings_update();
}

/* ---------- 搭建 ---------- */

static void build(void)
{
    lv_display_t *disp = gw_display_get();
    lv_theme_t *th = lv_theme_default_init(disp, UI_C_ACCENT, UI_C_OK, true, &font_cn_16);
    lv_display_set_theme(disp, th);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, UI_C_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 状态栏 */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, UI_W, UI_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_C_BG, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_bar_wifi = ui_label(bar, &font_cn_16, UI_C_SUB, "");
    lv_obj_set_width(s_bar_wifi, 100);
    lv_label_set_long_mode(s_bar_wifi, LV_LABEL_LONG_DOT);
    lv_obj_align(s_bar_wifi, LV_ALIGN_LEFT_MID, 0, 0);
    s_bar_title = ui_label(bar, &font_cn_16, UI_C_TEXT, "");
    lv_obj_align(s_bar_title, LV_ALIGN_CENTER, 0, 0);
    s_bar_right = ui_label(bar, &font_cn_16, UI_C_SUB, "");
    lv_obj_align(s_bar_right, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 页面 */
    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, UI_W, UI_BODY_H);
    lv_obj_set_pos(s_tv, 0, UI_BAR_H);
    lv_obj_set_style_bg_color(s_tv, UI_C_BG, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < PAGE_COUNT - 1 ? LV_DIR_RIGHT : 0);
        s_tiles[i] = lv_tileview_add_tile(s_tv, i, 0, dir);
        lv_obj_set_style_pad_all(s_tiles[i], 8, 0);
        lv_obj_set_style_pad_bottom(s_tiles[i], 14, 0);  /* 给页码点留位置 */
        lv_obj_set_style_pad_row(s_tiles[i], 8, 0);
        lv_obj_set_flex_flow(s_tiles[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(s_tiles[i], LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_tiles[i], LV_SCROLLBAR_MODE_ACTIVE);
    }
    lv_obj_add_event_cb(s_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ui_overview_create(s_tiles[0]);
    ui_nodes_create(s_tiles[1]);
    ui_fw_create(s_tiles[2]);
    ui_settings_create(s_tiles[3]);

    /* 页码点 */
    lv_obj_t *dots = lv_obj_create(scr);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 8);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 6, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_dots[i] = lv_obj_create(dots);
        lv_obj_set_size(s_dots[i], 6, 6);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* 提示条 */
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x333a44), 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_radius(s_toast, 16, 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 6, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    ui_label(s_toast, &font_cn_16, UI_C_TEXT, "");
    ui_set_hidden(s_toast, true);

    ui_overlay_create();

    s_blocker = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_blocker, UI_W, UI_H);
    lv_obj_set_style_bg_opa(s_blocker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_blocker, 0, 0);
    lv_obj_add_flag(s_blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_blocker, blocker_cb, LV_EVENT_PRESSED, NULL);
    ui_set_hidden(s_blocker, true);

    update_page_marks();
}

void ui_start(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, "off_s", &v) == ESP_OK) {
            s_screen_off_s = v;
        }
        nvs_close(h);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(g_ui.fw_version, app->version, sizeof(g_ui.fw_version));
    strlcpy(g_ui.partition, esp_ota_get_running_partition()->label, sizeof(g_ui.partition));
    refresh_model();

    if (!gw_display_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock timeout");
        return;
    }
    build();
    refresh_timer_cb(NULL);
    lv_timer_create(refresh_timer_cb, REFRESH_MS, NULL);
    gw_display_unlock();
}

void ui_on_key(gw_key_t key, gw_key_event_t ev)
{
    if (ev != GW_KEY_EV_PRESS) {
        return;
    }
    s_key_wake = true;
    if (key == GW_KEY_VOL_UP) {
        s_key_page_delta = -1;
    } else if (key == GW_KEY_VOL_DOWN) {
        s_key_page_delta = 1;
    }
}

void ui_request_bind_confirm(void)
{
    s_key_wake = true;
    s_req_bind = true;
}

void ui_request_prov_confirm(void)
{
    s_key_wake = true;
    s_req_prov_confirm = true;
}

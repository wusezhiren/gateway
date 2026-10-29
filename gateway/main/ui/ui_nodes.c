/* 节点列表页与节点详情（全屏，右滑或返回键关闭） */
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "ui_priv.h"

/* ================= 列表 ================= */

typedef struct {
    lv_obj_t *row;
    lv_obj_t *dot;
    lv_obj_t *name;
    lv_obj_t *sub;
    lv_obj_t *rssi;
} node_row_t;

static node_row_t s_rows[GW_MAX_NODES];
static lv_obj_t *s_empty;

static void open_detail(int idx);

static void row_cb(lv_event_t *e)
{
    open_detail((int)(intptr_t)lv_event_get_user_data(e));
}

void ui_nodes_create(lv_obj_t *tile)
{
    s_empty = ui_label(tile, &font_cn_16, UI_C_SUB, "还没有节点\n\n到“设置”页点击“开始配对”，再给节点上电");
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, lv_pct(100));
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_empty, 40, 0);

    /* 行预先建好，刷新时只改文字和显隐，避免滚动中重建 */
    for (int i = 0; i < GW_MAX_NODES; i++) {
        node_row_t *r = &s_rows[i];
        r->row = lv_obj_create(tile);
        lv_obj_set_size(r->row, lv_pct(100), 50);
        lv_obj_set_style_bg_color(r->row, UI_C_CARD, 0);
        lv_obj_set_style_bg_color(r->row, UI_C_LINE, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r->row, 0, 0);
        lv_obj_set_style_radius(r->row, 10, 0);
        lv_obj_set_style_pad_hor(r->row, 10, 0);
        lv_obj_set_style_pad_ver(r->row, 4, 0);
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        r->dot = lv_obj_create(r->row);
        lv_obj_set_size(r->dot, 10, 10);
        lv_obj_set_style_radius(r->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(r->dot, 0, 0);
        lv_obj_align(r->dot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(r->dot, LV_OBJ_FLAG_CLICKABLE);

        r->name = ui_label(r->row, &font_cn_16, UI_C_TEXT, "");
        lv_obj_align(r->name, LV_ALIGN_TOP_LEFT, 18, 0);
        lv_obj_set_width(r->name, 170);
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);
        r->sub = ui_label(r->row, &font_cn_16, UI_C_SUB, "");
        lv_obj_align(r->sub, LV_ALIGN_BOTTOM_LEFT, 18, 0);
        r->rssi = ui_label(r->row, &font_cn_16, UI_C_SUB, "");
        lv_obj_align(r->rssi, LV_ALIGN_RIGHT_MID, 0, 0);
        ui_set_hidden(r->row, true);
    }
}

void ui_nodes_update(void)
{
    ui_set_hidden(s_empty, g_ui.node_total > 0);
    for (int i = 0; i < GW_MAX_NODES; i++) {
        const gw_node_t *n = &g_ui.nodes[i];
        node_row_t *r = &s_rows[i];
        ui_set_hidden(r->row, !n->used);
        if (!n->used) {
            continue;
        }
        ui_bg_color(r->dot, n->online ? UI_C_OK : UI_C_SUB);
        ui_label_text(r->name, n->name);
        ui_label_fmt(r->sub, "v%s · %d 通道", n->fw_version, n->n_ch);
        if (n->online) {
            ui_label_fmt(r->rssi, "%s " LV_SYMBOL_RIGHT, ui_rssi_text(n->rssi));
        } else {
            ui_label_text(r->rssi, "离线 " LV_SYMBOL_RIGHT);
        }
    }
}

/* ================= 详情 ================= */

typedef struct {
    lv_obj_t *ctrl;     /* switch 或 slider */
    lv_obj_t *val;      /* PWM / 彩灯亮度百分比 */
    lv_obj_t *bri;      /* 彩灯亮度滑条 */
    lv_obj_t *sw[8];    /* 彩灯颜色色块 */
    uint16_t rgb;       /* 彩灯当前值（MP_RGB_*） */
    uint8_t type;
    int64_t hold_until; /* 刚操作过：节点回报新值前不用旧值覆盖，免得开关弹回去 */
} ch_ui_t;

#define HOLD_US (1500 * 1000)

static lv_obj_t *s_detail;
static int s_detail_idx = -1;
static lv_obj_t *s_dt_title, *s_dt_state, *s_dt_info, *s_dt_chbox;
static ch_ui_t s_ch[MP_MAX_CH];
static uint8_t s_dt_n_ch;
static mp_ch_desc_t s_dt_desc[MP_MAX_CH];

static void close_detail(void)
{
    if (s_detail) {
        lv_obj_delete(s_detail);
        s_detail = NULL;
    }
    s_detail_idx = -1;
}

static void back_cb(lv_event_t *e)
{
    close_detail();
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        close_detail();
    }
}

static void send_set(int ch, uint16_t value)
{
    s_ch[ch].hold_until = esp_timer_get_time() + HOLD_US;
    esp_err_t err = gw_link_set(s_detail_idx, ch, value);
    if (err == ESP_ERR_INVALID_STATE) {
        ui_toast("节点离线，无法控制");
    } else if (err != ESP_OK) {
        ui_toast("操作失败");
    }
}

static void switch_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    send_set(ch, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
}

static void slider_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(s);
    ui_label_fmt(s_ch[ch].val, "%ld%%", (long)(v * 100 / s_dt_desc[ch].max));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        send_set(ch, (uint16_t)v);  /* 拖动中只更新显示，松手才下发 */
    }
}

/* 彩灯色块：白光 + 7 个色相（MP_RGB_HUES 份里的序号） */
static const uint8_t RGB_SWATCH[8] = {MP_RGB_WHITE, 0, 20, 40, 80, 120, 160, 200};

static lv_color_t rgb_swatch_color(uint8_t color)
{
    return color == MP_RGB_WHITE ? lv_color_white() : lv_color_hsv_to_rgb(color * 360 / MP_RGB_HUES, 100, 100);
}

static void rgb_show(int ch, uint16_t v)
{
    unsigned bri = MP_RGB_BRI(v) ? MP_RGB_BRI(v) : 100;
    s_ch[ch].rgb = v;
    if (v & MP_RGB_ON) {
        lv_obj_add_state(s_ch[ch].ctrl, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_ch[ch].ctrl, LV_STATE_CHECKED);
    }
    /* 关灯时滑条显示 0，亮度仍记在取值里 */
    unsigned shown = (v & MP_RGB_ON) ? bri : 0;
    lv_slider_set_value(s_ch[ch].bri, shown, LV_ANIM_OFF);
    ui_label_fmt(s_ch[ch].val, "%u%%", shown);
    for (int k = 0; k < 8; k++) {
        lv_obj_set_style_border_color(s_ch[ch].sw[k], RGB_SWATCH[k] == MP_RGB_COLOR(v) ? UI_C_ACCENT : UI_C_LINE, 0);
    }
}

static void rgb_send(int ch, uint16_t v)
{
    rgb_show(ch, v);
    send_set(ch, v);
}

static void rgb_switch_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    rgb_send(ch, on ? s_ch[ch].rgb | MP_RGB_ON : s_ch[ch].rgb & ~MP_RGB_ON);
}

static void rgb_bri_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    int32_t v = lv_slider_get_value(lv_event_get_target(e));
    ui_label_fmt(s_ch[ch].val, "%ld%%", (long)v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        /* 拖到 0 关灯并保留原亮度，拖到其他值顺便开灯 */
        rgb_send(ch, v == 0 ? s_ch[ch].rgb & ~MP_RGB_ON : MP_RGB_PACK(1, v, MP_RGB_COLOR(s_ch[ch].rgb)));
    }
}

static void rgb_color_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t color = (uint8_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    unsigned bri = MP_RGB_BRI(s_ch[ch].rgb) ? MP_RGB_BRI(s_ch[ch].rgb) : 100;
    rgb_send(ch, MP_RGB_PACK(1, bri, color));
}

/* 彩灯一行：标题 + 开关，亮度滑条，颜色色块 */
static void build_rgb(lv_obj_t *row, int i)
{
    lv_obj_t *l = ui_label(row, &font_cn_16, UI_C_TEXT, "");
    ui_label_fmt(l, "彩灯 %d", i + 1);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 4);
    s_ch[i].val = ui_label(row, &font_cn_16, UI_C_SUB, "");
    lv_obj_align(s_ch[i].val, LV_ALIGN_TOP_LEFT, 64, 4);
    s_ch[i].ctrl = lv_switch_create(row);
    lv_obj_align(s_ch[i].ctrl, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(s_ch[i].ctrl, rgb_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);

    s_ch[i].bri = lv_slider_create(row);
    lv_obj_set_width(s_ch[i].bri, lv_pct(94));
    lv_obj_align(s_ch[i].bri, LV_ALIGN_TOP_MID, 0, 42);
    lv_slider_set_range(s_ch[i].bri, 0, 100);
    lv_obj_add_event_cb(s_ch[i].bri, rgb_bri_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(s_ch[i].bri, rgb_bri_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_set_size(box, lv_pct(100), 32);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int k = 0; k < 8; k++) {
        lv_obj_t *b = lv_obj_create(box);
        lv_obj_set_size(b, 24, 24);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, rgb_swatch_color(RGB_SWATCH[k]), 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_border_color(b, UI_C_LINE, 0);
        lv_obj_set_ext_click_area(b, 3);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(b, (void *)(intptr_t)RGB_SWATCH[k]);
        lv_obj_add_event_cb(b, rgb_color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_ch[i].sw[k] = b;
    }
    lv_obj_set_height(row, 100);
}

static void do_unpair(void *arg)
{
    int idx = s_detail_idx;
    close_detail();
    if (gw_link_unpair(idx) == ESP_OK) {
        ui_toast("已解绑");
    }
}

static void unpair_cb(lv_event_t *e)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "解绑“%s”后需要重新配对才能使用。", g_ui.nodes[s_detail_idx].name);
    ui_confirm("解绑节点", msg, "解绑", UI_C_ERR, do_unpair, NULL);
}

static void build_channels(const gw_node_t *n)
{
    lv_obj_clean(s_dt_chbox);
    s_dt_n_ch = n->n_ch;
    memcpy(s_dt_desc, n->ch, sizeof(s_dt_desc));
    if (n->n_ch == 0) {
        ui_label(s_dt_chbox, &font_cn_16, UI_C_SUB, "该节点没有可控制的通道");
    }
    for (int i = 0; i < n->n_ch; i++) {
        lv_obj_t *row = lv_obj_create(s_dt_chbox);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        memset(&s_ch[i], 0, sizeof(s_ch[i]));
        s_ch[i].type = n->ch[i].type;
        if (n->ch[i].type == MP_CH_RELAY) {
            lv_obj_t *l = ui_label(row, &font_cn_16, UI_C_TEXT, "");
            ui_label_fmt(l, "开关 %d", i + 1);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
            s_ch[i].ctrl = lv_switch_create(row);
            lv_obj_align(s_ch[i].ctrl, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_add_event_cb(s_ch[i].ctrl, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
            s_ch[i].val = NULL;
        } else if (n->ch[i].type == MP_CH_RGB) {
            build_rgb(row, i);
        } else {
            lv_obj_t *l = ui_label(row, &font_cn_16, UI_C_TEXT, "");
            ui_label_fmt(l, "调光 %d", i + 1);
            s_ch[i].val = ui_label(row, &font_cn_16, UI_C_SUB, "");
            lv_obj_align(s_ch[i].val, LV_ALIGN_TOP_RIGHT, 0, 0);
            s_ch[i].ctrl = lv_slider_create(row);
            lv_obj_set_width(s_ch[i].ctrl, lv_pct(94));
            lv_obj_align(s_ch[i].ctrl, LV_ALIGN_TOP_MID, 0, 30);
            lv_slider_set_range(s_ch[i].ctrl, 0, n->ch[i].max ? n->ch[i].max : 1);
            lv_obj_set_height(row, 56);
            lv_obj_add_event_cb(s_ch[i].ctrl, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(s_ch[i].ctrl, slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
        }
    }
}

static void open_detail(int idx)
{
    const gw_node_t *n = &g_ui.nodes[idx];
    if (!n->used) {
        return;
    }
    close_detail();
    s_detail_idx = idx;

    /* 盖在页面上方（状态栏以下），保留状态栏 */
    s_detail = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_detail, UI_W, UI_BODY_H);
    lv_obj_set_pos(s_detail, 0, UI_BAR_H);
    lv_obj_set_style_bg_color(s_detail, UI_C_BG, 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_radius(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 8, 0);
    lv_obj_set_style_pad_row(s_detail, 8, 0);
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);
    lv_obj_add_event_cb(s_detail, gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *head = lv_obj_create(s_detail);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back = ui_button(head, LV_SYMBOL_LEFT, UI_C_LINE, back_cb, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(back, 0), &lv_font_montserrat_16, 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    s_dt_title = ui_label(head, &font_cn_24, UI_C_TEXT, n->name);
    lv_obj_set_width(s_dt_title, 150);
    lv_label_set_long_mode(s_dt_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_dt_title, LV_ALIGN_LEFT_MID, 52, 0);
    s_dt_state = ui_label(head, &font_cn_16, UI_C_SUB, "");
    lv_obj_align(s_dt_state, LV_ALIGN_RIGHT_MID, 0, 0);

    s_dt_chbox = ui_card(s_detail);
    build_channels(n);

    lv_obj_t *info = ui_card(s_detail);
    s_dt_info = ui_label(info, &font_cn_16, UI_C_SUB, "");
    lv_label_set_long_mode(s_dt_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_dt_info, lv_pct(100));

    lv_obj_t *row = lv_obj_create(s_detail);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *up = ui_button(row, "升级固件", UI_C_ACCENT, NULL, NULL);
    lv_obj_set_flex_grow(up, 1);
    lv_obj_add_state(up, LV_STATE_DISABLED);    /* 节点 OTA（阶段 3b）完成后启用 */
    lv_obj_t *del = ui_button(row, "解绑", UI_C_ERR, unpair_cb, NULL);
    lv_obj_set_flex_grow(del, 1);

    ui_node_detail_update();
}

static const char *chip_name(uint8_t chip)
{
    switch (chip) {
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32S3: return "ESP32-S3";
    default:           return "ESP32";
    }
}

void ui_node_detail_update(void)
{
    if (!s_detail) {
        return;
    }
    const gw_node_t *n = &g_ui.nodes[s_detail_idx];
    if (!n->used) {
        close_detail();     /* 已被解绑（如 App 操作） */
        return;
    }
    if (n->n_ch != s_dt_n_ch || memcmp(n->ch, s_dt_desc, sizeof(mp_ch_desc_t) * n->n_ch) != 0) {
        build_channels(n);  /* 节点固件升级后通道布局变了 */
    }

    ui_label_text(s_dt_title, n->name);
    ui_label_text(s_dt_state, n->online ? "在线" : "离线");
    ui_text_color(s_dt_state, n->online ? UI_C_OK : UI_C_SUB);
    ui_label_fmt(s_dt_info, "%s  v%s\n" MACSTR "\n信号 %s (%d dBm)  运行 %lu 分钟",
                          chip_name(n->chip), n->fw_version, MAC2STR(n->mac), ui_rssi_text(n->rssi), n->rssi,
                          (unsigned long)(n->uptime_s / 60));

    for (int i = 0; i < s_dt_n_ch; i++) {
        lv_obj_t *c = s_ch[i].ctrl;
        if (n->online) {
            lv_obj_remove_state(c, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(c, LV_STATE_DISABLED);
        }
        if (s_ch[i].bri) {
            if (n->online) {
                lv_obj_remove_state(s_ch[i].bri, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(s_ch[i].bri, LV_STATE_DISABLED);
            }
        }
        if (lv_obj_has_state(c, LV_STATE_PRESSED) || (s_ch[i].bri && lv_obj_has_state(s_ch[i].bri, LV_STATE_PRESSED)) ||
            esp_timer_get_time() < s_ch[i].hold_until) {
            continue;   /* 正在操作，不用节点上报值覆盖 */
        }
        if (s_ch[i].type == MP_CH_RGB) {
            rgb_show(i, n->value[i]);
        } else if (s_ch[i].type == MP_CH_RELAY) {
            if (n->value[i]) {
                lv_obj_add_state(c, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(c, LV_STATE_CHECKED);
            }
        } else {
            lv_slider_set_value(c, n->value[i], LV_ANIM_OFF);
            ui_label_fmt(s_ch[i].val, "%ld%%",
                                  (long)(n->value[i] * 100 / (s_dt_desc[i].max ? s_dt_desc[i].max : 1)));
        }
    }
}

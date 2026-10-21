/* 全屏层：配网（热点二维码 + 进度）、网关 OTA 进度 */
#include <stdio.h>
#include <string.h>
#include "ui_priv.h"

static lv_obj_t *s_prov, *s_prov_qr, *s_prov_ssid, *s_prov_pass, *s_prov_state, *s_prov_left, *s_prov_btn;
static char s_qr_text[128];

static lv_obj_t *s_ota, *s_ota_title, *s_ota_bar, *s_ota_pct;

static lv_obj_t *fullscreen(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 10, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);  /* 挡住下层 */
    ui_set_hidden(o, true);
    return o;
}

static void prov_cancel_cb(lv_event_t *e)
{
    gw_prov_stop();
}

void ui_overlay_create(void)
{
    /* 配网：左二维码（扫码连热点），右文字 */
    s_prov = fullscreen();
    ui_label(s_prov, &font_cn_24, UI_C_TEXT, "配网模式");
    s_prov_qr = lv_qrcode_create(s_prov);
    lv_qrcode_set_size(s_prov_qr, 116);
    lv_qrcode_set_dark_color(s_prov_qr, lv_color_black());
    lv_qrcode_set_light_color(s_prov_qr, lv_color_white());
    lv_obj_set_style_border_color(s_prov_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_prov_qr, 4, 0);
    lv_obj_align(s_prov_qr, LV_ALIGN_LEFT_MID, 0, 10);

    lv_obj_t *col = lv_obj_create(s_prov);
    lv_obj_set_size(col, 128, 150);
    lv_obj_align(col, LV_ALIGN_RIGHT_MID, 0, 6);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(col, &font_cn_16, UI_C_SUB, "热点");
    s_prov_ssid = ui_label(col, &font_cn_16, UI_C_TEXT, "");
    ui_label(col, &font_cn_16, UI_C_SUB, "密码");
    s_prov_pass = ui_label(col, &font_cn_16, UI_C_TEXT, "");
    s_prov_state = ui_label(col, &font_cn_16, UI_C_ACCENT, "");
    lv_obj_set_width(s_prov_state, lv_pct(100));
    lv_label_set_long_mode(s_prov_state, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(s_prov_state, 6, 0);

    s_prov_left = ui_label(s_prov, &font_cn_16, UI_C_SUB, "");
    lv_obj_align(s_prov_left, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_prov_btn = ui_button(s_prov, "取消", UI_C_LINE, prov_cancel_cb, NULL);
    lv_obj_align(s_prov_btn, LV_ALIGN_TOP_RIGHT, 0, -4);

    /* 网关 OTA */
    s_ota = fullscreen();
    s_ota_title = ui_label(s_ota, &font_cn_24, UI_C_TEXT, "");
    lv_obj_align(s_ota_title, LV_ALIGN_CENTER, 0, -40);
    s_ota_bar = lv_bar_create(s_ota);
    lv_obj_set_size(s_ota_bar, 220, 12);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_obj_align(s_ota_bar, LV_ALIGN_CENTER, 0, 0);
    s_ota_pct = ui_label(s_ota, &font_cn_16, UI_C_SUB, "");
    lv_obj_align(s_ota_pct, LV_ALIGN_CENTER, 0, 30);
}

static const char *prov_state_text(const gw_prov_status_t *p)
{
    static char buf[96];
    switch (p->state) {
    case GW_PROV_WAITING:
        return "用手机扫码或连接热点";
    case GW_PROV_CLIENT:
        return "手机已连接\n请在弹出的页面中选择 Wi-Fi";
    case GW_PROV_CONNECTING:
        snprintf(buf, sizeof(buf), "正在连接\n%s …", p->target);
        return buf;
    case GW_PROV_DONE:
        snprintf(buf, sizeof(buf), "配网成功\n%s", p->ip);
        return buf;
    case GW_PROV_FAILED:
        switch (p->result) {
        case GW_TRY_BAD_PASSWORD: return "密码错误\n请在手机上重试";
        case GW_TRY_NO_AP:        return "找不到该网络\n请确认是 2.4GHz";
        case GW_TRY_NO_IP:        return "未获取到 IP\n请检查路由器";
        default:                  return "连接失败\n请在手机上重试";
        }
    default:
        return "";
    }
}

bool ui_overlay_update(void)
{
    /* OTA 优先 */
    const gw_ota_status_t *o = &g_ui.ota;
    bool ota_on = o->state == GW_OTA_RECEIVING || o->state == GW_OTA_REBOOTING;
    ui_set_hidden(s_ota, !ota_on);
    if (ota_on) {
        lv_obj_move_foreground(s_ota);
        int pct = o->total ? (int)((uint64_t)o->received * 100 / o->total) : 0;
        if (o->state == GW_OTA_REBOOTING) {
            ui_label_text(s_ota_title, "校验通过，正在重启");
            pct = 100;
        } else {
            if (o->version[0]) {
                ui_label_fmt(s_ota_title, "升级网关 v%s", o->version);
            } else {
                ui_label_text(s_ota_title, "升级网关");
            }
        }
        lv_bar_set_value(s_ota_bar, pct, LV_ANIM_OFF);
        ui_label_fmt(s_ota_pct, "%d%%  请勿断电", pct);
    }

    const gw_prov_status_t *p = &g_ui.prov;
    bool prov_on = p->state != GW_PROV_OFF && !ota_on;
    ui_set_hidden(s_prov, !prov_on);
    if (prov_on) {
        char qr[128];
        snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", p->ap_ssid, p->ap_pass);
        if (strcmp(qr, s_qr_text) != 0) {
            strlcpy(s_qr_text, qr, sizeof(s_qr_text));
            lv_qrcode_update(s_prov_qr, s_qr_text, strlen(s_qr_text));
        }
        ui_label_text(s_prov_ssid, p->ap_ssid);
        ui_label_text(s_prov_pass, p->ap_pass);
        ui_label_text(s_prov_state, prov_state_text(p));
        ui_text_color(s_prov_state,
                      p->state == GW_PROV_DONE ? UI_C_OK :
                      p->state == GW_PROV_FAILED ? UI_C_ERR : UI_C_ACCENT);
        if (p->state == GW_PROV_DONE) {
            ui_label_fmt(s_prov_left, "%lu 秒后关闭热点", (unsigned long)p->remaining_s);
        } else {
            ui_label_fmt(s_prov_left, "%lu 秒无操作自动退出", (unsigned long)p->remaining_s);
        }
        ui_set_hidden(s_prov_btn, p->state == GW_PROV_DONE);
    }
    return ota_on || prov_on;
}

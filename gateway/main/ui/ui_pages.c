/* 概览、固件、设置三个页面 */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ui_priv.h"

/* 卡片里的一行：左边灰色说明，右边数值 */
static lv_obj_t *kv_row(lv_obj_t *card, const char *key)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(row, &font_cn_16, UI_C_SUB, key);
    lv_obj_t *v = ui_label(row, &font_cn_16, UI_C_TEXT, "-");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_max_width(v, 170, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    return v;
}

/* ================= 概览 ================= */

static lv_obj_t *s_ov_net_title, *s_ov_ip, *s_ov_rssi, *s_ov_ch, *s_ov_cloud;
static lv_obj_t *s_ov_nodes, *s_ov_fw, *s_ov_uptime;

static lv_obj_t *stat_tile(lv_obj_t *parent, const char *title, lv_obj_t **value)
{
    lv_obj_t *c = ui_card(parent);
    lv_obj_set_width(c, 126);
    lv_obj_set_flex_grow(c, 1);
    ui_label(c, &font_cn_16, UI_C_SUB, title);
    *value = ui_label(c, &font_cn_24, UI_C_TEXT, "-");
    return c;
}

void ui_overview_create(lv_obj_t *tile)
{
    lv_obj_t *net = ui_card(tile);
    s_ov_net_title = ui_label(net, &font_cn_24, UI_C_TEXT, "");
    lv_obj_set_width(s_ov_net_title, lv_pct(100));
    lv_label_set_long_mode(s_ov_net_title, LV_LABEL_LONG_DOT);
    s_ov_ip = kv_row(net, "IP 地址");
    s_ov_rssi = kv_row(net, "信号");
    s_ov_ch = kv_row(net, "信道");
    s_ov_cloud = kv_row(net, "云端");

    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    stat_tile(row, "在线节点", &s_ov_nodes);
    stat_tile(row, "网关固件", &s_ov_fw);

    s_ov_uptime = ui_label(tile, &font_cn_16, UI_C_SUB, "");
}

void ui_overview_update(void)
{
    const gw_wifi_status_t *w = &g_ui.wifi;
    if (w->connected) {
        lv_label_set_text(s_ov_net_title, w->ssid);
        lv_obj_set_style_text_color(s_ov_net_title, UI_C_OK, 0);
        lv_label_set_text(s_ov_ip, w->ip);
        lv_label_set_text_fmt(s_ov_rssi, "%s  %d dBm", ui_rssi_text(w->rssi), w->rssi);
    } else {
        lv_label_set_text(s_ov_net_title, w->prov ? "配网中" : w->configured ? "正在连接路由器…" : "未配置路由器");
        lv_obj_set_style_text_color(s_ov_net_title, w->configured ? UI_C_WARN : UI_C_SUB, 0);
        lv_label_set_text(s_ov_ip, w->configured ? w->ssid : "长按 REC 键配网");
        lv_label_set_text(s_ov_rssi, "-");
    }
    lv_label_set_text_fmt(s_ov_ch, "%d", w->channel);
    const gw_cloud_status_t *c = &g_ui.cloud;
    lv_label_set_text(s_ov_cloud, !c->configured ? "未配置" : c->connected ? "已连接"
                                  : c->error[0] ? c->error : "连接中…");
    lv_obj_set_style_text_color(s_ov_cloud, !c->configured ? UI_C_SUB : c->connected ? UI_C_OK : UI_C_WARN, 0);

    lv_label_set_text_fmt(s_ov_nodes, "%d / %d", g_ui.node_online, g_ui.node_total);
    lv_obj_set_style_text_color(s_ov_nodes,
                                g_ui.node_total && g_ui.node_online < g_ui.node_total ? UI_C_WARN : UI_C_TEXT, 0);
    lv_label_set_text_fmt(s_ov_fw, "v%s", g_ui.fw_version);

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_ov_uptime, "已运行 %lu 天 %02lu:%02lu:%02lu", (unsigned long)(up / 86400),
                          (unsigned long)(up / 3600 % 24), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
}

/* ================= 固件 ================= */

static lv_obj_t *s_fw_ver, *s_fw_part, *s_fw_state, *s_fw_bar;
static lv_obj_t *s_fw_node_c3, *s_fw_node_s3;

void ui_fw_create(lv_obj_t *tile)
{
    lv_obj_t *gw = ui_card(tile);
    ui_label(gw, &font_cn_16, UI_C_SUB, "网关固件");
    s_fw_ver = ui_label(gw, &font_cn_24, UI_C_TEXT, "");
    s_fw_part = kv_row(gw, "运行分区");
    s_fw_state = kv_row(gw, "上次升级");
    s_fw_bar = lv_bar_create(gw);
    lv_obj_set_size(s_fw_bar, lv_pct(100), 8);
    lv_bar_set_range(s_fw_bar, 0, 100);
    ui_set_hidden(s_fw_bar, true);

    lv_obj_t *node = ui_card(tile);
    ui_label(node, &font_cn_16, UI_C_SUB, "节点固件");
    s_fw_node_c3 = kv_row(node, "ESP32-C3");
    s_fw_node_s3 = kv_row(node, "ESP32-S3");
    lv_obj_t *btn = ui_button(node, "全部升级", UI_C_ACCENT, NULL, NULL);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_add_state(btn, LV_STATE_DISABLED);   /* 节点 OTA（阶段 3b）完成后启用 */

    lv_obj_t *hint = ui_label(tile, &font_cn_16, UI_C_SUB, "通过 App 或 tools/ota_upload.py 上传固件");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
}

void ui_fw_update(void)
{
    const gw_ota_status_t *o = &g_ui.ota;
    lv_label_set_text_fmt(s_fw_ver, "v%s", g_ui.fw_version);
    lv_label_set_text(s_fw_part, g_ui.partition);
    switch (o->state) {
    case GW_OTA_IDLE:
        lv_label_set_text(s_fw_state, "无");
        break;
    case GW_OTA_RECEIVING:
        lv_label_set_text_fmt(s_fw_state, "接收 v%s", o->version);
        break;
    case GW_OTA_REBOOTING:
        lv_label_set_text_fmt(s_fw_state, "v%s 即将重启", o->version);
        break;
    case GW_OTA_FAILED:
        lv_label_set_text_fmt(s_fw_state, "失败：%s", o->error);
        break;
    }
    bool receiving = o->state == GW_OTA_RECEIVING && o->total;
    ui_set_hidden(s_fw_bar, !receiving);
    if (receiving) {
        lv_bar_set_value(s_fw_bar, (int32_t)((uint64_t)o->received * 100 / o->total), LV_ANIM_OFF);
    }
    lv_label_set_text(s_fw_node_c3, "未上传");
    lv_label_set_text(s_fw_node_s3, "未上传");
}

/* ================= 设置 ================= */

static lv_obj_t *s_set_pair_btn, *s_set_pair_lbl, *s_set_pair_hint;
static lv_obj_t *s_set_dd;

static const uint32_t SCREEN_OFF_OPTS[] = {30, 60, 300, 0};

static void pair_cb(lv_event_t *e)
{
    if (g_ui.pair_left) {
        gw_link_pair(0);
    } else {
        gw_link_pair(60);
    }
    ui_settings_update();
}

static void do_prov(void *arg)
{
    gw_prov_start();
}

void ui_settings_start_prov(void)
{
    if (g_ui.prov.state != GW_PROV_OFF) {
        return;
    }
    if (g_ui.wifi.connected) {
        ui_confirm("重新配网", "网关已联网，进入配网会断开当前 Wi-Fi，配网失败或取消后自动恢复。", "进入配网",
                   UI_C_ACCENT, do_prov, NULL);
    } else {
        gw_prov_start();
    }
}

static void prov_cb(lv_event_t *e)
{
    ui_settings_start_prov();
}

static void dd_cb(lv_event_t *e)
{
    uint32_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel < sizeof(SCREEN_OFF_OPTS) / sizeof(SCREEN_OFF_OPTS[0])) {
        ui_set_screen_off_s(SCREEN_OFF_OPTS[sel]);
    }
}

static void do_reboot(void *arg)
{
    esp_restart();
}

static void reboot_cb(lv_event_t *e)
{
    ui_confirm("重启网关", "节点会短暂离线，重启后自动恢复。", "重启", UI_C_ERR, do_reboot, NULL);
}

void ui_settings_create(lv_obj_t *tile)
{
    /* 配对 */
    lv_obj_t *pair = ui_card(tile);
    ui_label(pair, &font_cn_16, UI_C_SUB, "添加节点");
    s_set_pair_btn = ui_button(pair, "", UI_C_ACCENT, pair_cb, NULL);
    lv_obj_set_width(s_set_pair_btn, lv_pct(100));
    s_set_pair_lbl = lv_obj_get_child(s_set_pair_btn, 0);
    s_set_pair_hint = ui_label(pair, &font_cn_16, UI_C_SUB, "节点首次上电后会自动加入");
    lv_label_set_long_mode(s_set_pair_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_set_pair_hint, lv_pct(100));

    /* 网络 */
    lv_obj_t *net = ui_card(tile);
    ui_label(net, &font_cn_16, UI_C_SUB, "网络");
    lv_obj_t *b = ui_button(net, "重新配网", UI_C_LINE, prov_cb, NULL);
    lv_obj_set_width(b, lv_pct(100));

    /* 屏幕 */
    lv_obj_t *scr = ui_card(tile);
    ui_label(scr, &font_cn_16, UI_C_SUB, "自动熄屏");
    s_set_dd = lv_dropdown_create(scr);
    lv_obj_set_width(s_set_dd, lv_pct(100));
    lv_dropdown_set_options(s_set_dd, "30 秒\n1 分钟\n5 分钟\n常亮");
    lv_obj_set_style_text_font(s_set_dd, &font_cn_16, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_set_dd), &font_cn_16, 0);
    uint32_t cur = ui_screen_off_s();
    for (uint32_t i = 0; i < sizeof(SCREEN_OFF_OPTS) / sizeof(SCREEN_OFF_OPTS[0]); i++) {
        if (SCREEN_OFF_OPTS[i] == cur) {
            lv_dropdown_set_selected(s_set_dd, i);
        }
    }
    lv_obj_add_event_cb(s_set_dd, dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 关于 */
    lv_obj_t *about = ui_card(tile);
    ui_label(about, &font_cn_16, UI_C_SUB, "关于");
    uint8_t mac[6];
    char buf[24];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, sizeof(buf), MACSTR, MAC2STR(mac));
    lv_label_set_text(kv_row(about, "MAC"), buf);
    lv_label_set_text(kv_row(about, "IDF"), esp_app_get_description()->idf_ver);
    b = ui_button(about, "重启网关", UI_C_ERR, reboot_cb, NULL);
    lv_obj_set_width(b, lv_pct(100));
}

void ui_settings_update(void)
{
    if (g_ui.pair_left) {
        lv_label_set_text_fmt(s_set_pair_lbl, "停止配对（%lu 秒）", (unsigned long)g_ui.pair_left);
        lv_obj_set_style_bg_color(s_set_pair_btn, UI_C_WARN, 0);
        lv_label_set_text(s_set_pair_hint, "配对中：给新节点上电，加入后会出现在节点页");
    } else {
        lv_label_set_text(s_set_pair_lbl, "开始配对（60 秒）");
        lv_obj_set_style_bg_color(s_set_pair_btn, UI_C_ACCENT, 0);
        lv_label_set_text(s_set_pair_hint, "已配对节点重启后会自动加入，无需再次配对");
    }
}

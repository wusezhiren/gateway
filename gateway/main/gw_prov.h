#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "gw_wifi.h"

/*
 * 配网：SoftAP + DNS 劫持（强制门户）+ HTTP 配网接口（网页和 App 共用，见 docs/api.md）。
 * 10 分钟无操作自动退出；配网成功后热点保留 30 秒再关闭。
 */

typedef enum {
    GW_PROV_OFF,
    GW_PROV_WAITING,        /* 热点已开，等待手机连接 */
    GW_PROV_CLIENT,         /* 有手机连上热点 */
    GW_PROV_CONNECTING,     /* 正在连接路由器 */
    GW_PROV_DONE,           /* 成功，热点即将关闭 */
    GW_PROV_FAILED,         /* 本次试连失败，可重试 */
} gw_prov_state_t;

typedef struct {
    gw_prov_state_t state;
    char ap_ssid[33];
    char ap_pass[65];
    char target[33];        /* 正在/最近试连的路由器 */
    gw_try_state_t result;  /* FAILED 时的原因 */
    char ip[16];            /* DONE 时网关拿到的 IP */
    int clients;            /* 连上热点的手机数 */
    uint32_t remaining_s;   /* 距自动关闭的秒数 */
} gw_prov_status_t;

typedef enum {
    GW_PROV_KEY_STARTED,
    GW_PROV_KEY_STOPPED,
    GW_PROV_KEY_NEED_CONFIRM,   /* 已联网，需在屏幕上确认后调用 gw_prov_start() */
} gw_prov_key_result_t;

/* 状态变化通知，在配网任务里调用，不要阻塞 */
typedef void (*gw_prov_cb_t)(const gw_prov_status_t *st);

esp_err_t gw_prov_init(gw_prov_cb_t cb);
esp_err_t gw_prov_start(void);
void gw_prov_stop(void);
void gw_prov_get_status(gw_prov_status_t *st);

/* REC 长按：未配网中且未联网则进入；已联网返回 NEED_CONFIRM；配网中则取消 */
gw_prov_key_result_t gw_prov_on_long_press(void);

/* App 访问网关 API 用的 token（首次调用时生成并存 NVS） */
const char *gw_prov_api_token(void);

const char *gw_prov_state_name(gw_prov_state_t s);
const char *gw_try_state_name(gw_try_state_t s);

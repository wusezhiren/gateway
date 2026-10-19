#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * 网关自升级：POST /api/ota/gateway（需 token），固件流式写入空闲 OTA 分区，
 * esp_ota_end 校验签名后切换启动分区并重启；新固件首次启动的确认/回滚见 main.c。
 */

typedef enum {
    GW_OTA_IDLE,
    GW_OTA_RECEIVING,
    GW_OTA_REBOOTING,   /* 校验通过，即将重启 */
    GW_OTA_FAILED,
} gw_ota_state_t;

typedef struct {
    gw_ota_state_t state;
    uint32_t received;
    uint32_t total;
    char version[32];   /* 正在/最近一次升级的目标版本 */
    char error[32];     /* FAILED 时的原因 */
} gw_ota_status_t;

/* 状态或进度变化（每 5%）时回调，在 HTTP 任务里执行，不要阻塞 */
typedef void (*gw_ota_cb_t)(const gw_ota_status_t *st);

esp_err_t gw_ota_register(httpd_handle_t httpd);
void gw_ota_set_cb(gw_ota_cb_t cb);
void gw_ota_get_status(gw_ota_status_t *st);
const char *gw_ota_state_name(gw_ota_state_t s);

/* 比较 "x.y.z" 版本号，返回 <0 / 0 / >0 */
int gw_ota_version_cmp(const char *a, const char *b);

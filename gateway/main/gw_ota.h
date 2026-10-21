#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"

/*
 * 网关自升级，两种来源：
 *  - 局域网上传：POST /api/ota/gateway（需 token）
 *  - 远程：云端 ota 命令带下载地址和 SHA-256，网关自己用 HTTPS 下载（gw_ota_start_url）
 * 固件流式写入空闲 OTA 分区，esp_ota_end 校验签名（下载的再核对 SHA-256）后切换启动分区并重启；
 * 新固件首次启动的确认/回滚见 main.c。
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

/*
 * 后台下载 url（必须 https）并升级，立即返回：NULL 表示已开始，否则为错误码
 * （bad_url / bad_sha256 / busy / no_mem）。进度和结果通过状态回调上报。
 * version 只用于进度显示，以镜像头里的版本为准。
 */
const char *gw_ota_start_url(const char *url, const char *sha256_hex, const char *version, bool force);
const char *gw_ota_state_name(gw_ota_state_t s);

/* 下载辅助（节点升级共用）：打开并跟随跳转，成功返回 NULL；读满 len 字节或到结尾，出错返回 -1 */
const char *gw_ota_http_open(esp_http_client_handle_t c, int64_t *len);
int gw_ota_http_read(esp_http_client_handle_t c, char *buf, int len);

/* 比较 "x.y.z" 版本号，返回 <0 / 0 / >0 */
int gw_ota_version_cmp(const char *a, const char *b);

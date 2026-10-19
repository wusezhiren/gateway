#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/*
 * 两种模式：
 *   AUTO  正常运行，用 NVS 里的凭据自动重连（指数退避），重试间隙停在上次的路由器信道
 *   PROV  配网中：开 SoftAP（APSTA），暂停自动重连，由配网模块调用 gw_wifi_try_connect() 试连
 */

/* 初始化 Wi-Fi；NVS 里有路由器凭据就去连，没有则停在 CONFIG_GW_DEFAULT_CHANNEL */
esp_err_t gw_wifi_init(void);

/* 保存凭据并重新连接；ssid 为 NULL 表示清除凭据（串口调试用） */
esp_err_t gw_wifi_set_credentials(const char *ssid, const char *password);

typedef struct {
    bool configured;
    bool connected;
    bool prov;              /* 处于配网模式 */
    char ssid[33];
    uint8_t channel;
    int8_t rssi;            /* 已连接时有效 */
    char ip[16];
} gw_wifi_status_t;

void gw_wifi_get_status(gw_wifi_status_t *st);

/* ---- 配网支持 ---- */

esp_err_t gw_wifi_prov_begin(const char *ap_ssid, const char *ap_password);

/* 退出配网：关闭 SoftAP，按 NVS 凭据恢复自动重连（试连成功时 NVS 已是新凭据） */
void gw_wifi_prov_end(void);

/* 阻塞扫描（约 2~3s），返回去重后按信号排序的 AP 数量 */
int gw_wifi_scan(wifi_ap_record_t *out, int max);

typedef enum {
    GW_TRY_IDLE,
    GW_TRY_CONNECTING,
    GW_TRY_OK,              /* 已连上并拿到 IP，凭据已保存 */
    GW_TRY_BAD_PASSWORD,
    GW_TRY_NO_AP,
    GW_TRY_NO_IP,
    GW_TRY_FAILED,
} gw_try_state_t;

/* 配网模式下异步试连，结果用 gw_wifi_try_state() 查询；password 为空表示开放网络 */
esp_err_t gw_wifi_try_connect(const char *ssid, const char *password);
gw_try_state_t gw_wifi_try_state(void);

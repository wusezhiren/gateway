#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * 节点固件升级，两种来源：
 *  - 网关自己下载：云端 / HTTP 的 node_ota 命令带下载地址和 SHA-256（gw_node_ota_start）
 *  - 局域网上传：POST /api/ota/node?mac=...（需 token）
 * 固件暂存到 node_fw 分区，经 ESP-NOW 分片推给节点，节点以新版本重新入网才算成功。
 * 同一时间只升级一个节点，且不与网关自身升级同时进行。
 */

typedef enum {
    GW_NODE_OTA_IDLE,
    GW_NODE_OTA_DOWNLOADING,    /* 下载或上传到暂存区 */
    GW_NODE_OTA_SENDING,        /* 推送给节点 */
    GW_NODE_OTA_REBOOTING,      /* 节点校验通过已重启，等它重新入网 */
    GW_NODE_OTA_DONE,
    GW_NODE_OTA_FAILED,
} gw_node_ota_state_t;

typedef struct {
    gw_node_ota_state_t state;
    uint8_t mac[6];
    uint32_t received;  /* 当前阶段进度 */
    uint32_t total;
    char version[32];   /* 目标版本（读到镜像头后才有） */
    char error[32];     /* FAILED 时的原因 */
} gw_node_ota_status_t;

/* 状态或进度变化（每 5%）时回调，不要阻塞 */
typedef void (*gw_node_ota_cb_t)(const gw_node_ota_status_t *st);

esp_err_t gw_node_ota_register(httpd_handle_t httpd);
void gw_node_ota_set_cb(gw_node_ota_cb_t cb);
void gw_node_ota_get_status(gw_node_ota_status_t *st);
const char *gw_node_ota_state_name(gw_node_ota_state_t s);
bool gw_node_ota_busy(void);

/*
 * 后台下载 url（必须 https）并升级节点 mac，立即返回：NULL 表示已开始，否则为错误码
 * （bad_url / bad_sha256 / no_such_node / offline / busy / no_mem）。
 */
const char *gw_node_ota_start(const uint8_t mac[6], const char *url, const char *sha256_hex, bool force);

/* gw_link 收到 MP_OTA_ACK 时调用 */
void gw_node_ota_on_ack(const uint8_t src[6], const uint8_t *data, int len);

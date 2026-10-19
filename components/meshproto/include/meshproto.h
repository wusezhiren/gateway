/*
 * 网关 <-> 节点 ESP-NOW 协议定义，网关和节点工程共用。
 * 详细说明见 docs/protocol.md。修改结构体时必须同步升级 MP_VERSION。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MAGIC            0xE5
#define MP_VERSION          1
#define MP_MAX_CH           8       /* 每个节点最多通道数 */
#define MP_NAME_LEN         16      /* 节点名，不含结尾 0 */
#define MP_VER_LEN          16      /* 固件版本字符串（esp_app_desc_t.version 截断） */
#define MP_PROJ_LEN         16      /* 工程名（esp_app_desc_t.project_name 截断） */
#define MP_HB_INTERVAL_MS   5000    /* 默认心跳周期 */
#define MP_HB_MISS_MAX      3       /* 连续丢失多少次心跳判定掉线 */

typedef enum {
    MP_JOIN_REQ  = 1,   /* 节点 -> 广播：请求入网，携带能力描述 */
    MP_JOIN_RSP  = 2,   /* 网关 -> 广播：入网应答，target 指定节点 */
    MP_HEARTBEAT = 3,   /* 节点 -> 网关：周期心跳，携带全部通道状态 */
    MP_HB_ACK    = 4,   /* 网关 -> 节点：心跳应答 */
    MP_SET       = 5,   /* 网关 -> 节点：设置通道值 */
    MP_STATE     = 6,   /* 节点 -> 网关：状态上报，ack_seq 对应 SET 的 seq */
    MP_LEAVE     = 7,   /* 网关 -> 节点：解除配对，节点清除网关信息 */
} mp_type_t;

typedef enum {
    MP_CH_RELAY = 1,    /* 开关量，取值 0/1 */
    MP_CH_PWM   = 2,    /* 调光/调速，取值 0..max */
} mp_ch_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  reserved;
    uint16_t seq;
} mp_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;      /* mp_ch_type_t */
    uint8_t  flags;     /* 预留 */
    uint16_t max;       /* 取值上限：RELAY 为 1 */
} mp_ch_desc_t;

typedef struct __attribute__((packed)) {
    mp_hdr_t     hdr;
    char         fw_version[MP_VER_LEN];    /* 不足补 0，满长时无结尾 0 */
    char         project[MP_PROJ_LEN];
    uint8_t      chip;                      /* esp_chip_model_t */
    char         name[MP_NAME_LEN];
    uint8_t      n_ch;
    mp_ch_desc_t ch[MP_MAX_CH];
} mp_join_req_t;

enum { MP_JOIN_OK = 0 };

typedef struct __attribute__((packed)) {
    mp_hdr_t hdr;
    uint8_t  target[6];         /* 应答发给哪个节点（广播帧） */
    uint8_t  status;            /* MP_JOIN_OK */
    uint8_t  channel;           /* 网关当前信道 */
    uint16_t hb_interval_ms;
} mp_join_rsp_t;

/* MP_HEARTBEAT 和 MP_STATE 共用 */
typedef struct __attribute__((packed)) {
    mp_hdr_t hdr;
    uint16_t ack_seq;           /* STATE 回应 SET 时填 SET.seq，否则 0 */
    uint32_t uptime_s;
    uint8_t  n_ch;
    uint16_t value[MP_MAX_CH];
} mp_state_t;

typedef struct __attribute__((packed)) {
    mp_hdr_t hdr;
} mp_hb_ack_t, mp_leave_t;

typedef struct __attribute__((packed)) {
    mp_hdr_t hdr;
    uint8_t  ch;
    uint16_t value;
} mp_set_t;

_Static_assert(sizeof(mp_join_req_t) <= ESP_NOW_MAX_DATA_LEN, "frame too large");

extern const uint8_t MP_BROADCAST[6];

void mp_hdr_init(mp_hdr_t *hdr, mp_type_t type, uint16_t seq);

/* 校验帧头和长度，合法返回帧类型，否则返回 0 */
int mp_parse(const uint8_t *data, int len);

/* 设置 PMK（启用加密时），在 esp_now_init() 之后调用 */
esp_err_t mp_espnow_setup(void);

/* 添加或更新一个 peer；encrypt 为 false 或未启用 MP_ENCRYPT 时不加密 */
esp_err_t mp_peer_upsert(const uint8_t mac[6], bool encrypt);

#ifdef __cplusplus
}
#endif

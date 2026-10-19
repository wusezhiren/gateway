#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "meshproto.h"

#define GW_MAX_NODES CONFIG_GW_MAX_NODES

typedef struct {
    bool         used;
    uint8_t      mac[6];
    char         name[MP_NAME_LEN + 1];
    char         fw_version[MP_VER_LEN + 1];
    char         project[MP_PROJ_LEN + 1];
    uint8_t      chip;          /* esp_chip_model_t */
    uint8_t      n_ch;
    mp_ch_desc_t ch[MP_MAX_CH];
    /* 运行时状态，不落盘 */
    bool         online;
    int8_t       rssi;
    uint32_t     uptime_s;
    int64_t      last_seen_us;
    uint16_t     value[MP_MAX_CH];      /* 节点上报的实际值 */
    uint16_t     desired[MP_MAX_CH];    /* 期望值，节点重新入网后与实际值不符则补发 */
    uint8_t      desired_mask;          /* desired 中有效的通道位图 */
    bool         resync;                /* 刚入网，等待首个状态上报做比对 */
} gw_node_t;

typedef enum {
    GW_EV_NODE_JOINED,      /* 入网（新配对或重连） */
    GW_EV_NODE_OFFLINE,
    GW_EV_NODE_STATE,       /* 通道值变化 */
    GW_EV_NODE_REMOVED,
    GW_EV_SET_FAILED,       /* SET 重试耗尽 */
} gw_event_t;

/* 回调在 link 任务里执行，不要阻塞 */
typedef void (*gw_event_cb_t)(gw_event_t ev, int idx);

esp_err_t gw_link_start(void);
void gw_link_set_event_cb(gw_event_cb_t cb);

/* 拷贝整张节点表（GW_MAX_NODES 项），idx 即数组下标，未使用项 used=false */
void gw_link_snapshot(gw_node_t out[GW_MAX_NODES]);

esp_err_t gw_link_set(int idx, uint8_t ch, uint16_t value);
esp_err_t gw_link_unpair(int idx);

/* 打开配对窗口 seconds 秒，0 立即关闭 */
void gw_link_pair(uint32_t seconds);
uint32_t gw_link_pair_remaining(void);

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "meshproto.h"

#define GW_MAX_NODES CONFIG_GW_MAX_NODES

_Static_assert(GW_MAX_NODES <= CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM,
               "GW_MAX_NODES exceeds ESP-NOW encrypted peer limit");

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
    uint32_t     joins;                 /* 本次开机以来的入网次数，升级时据此判断节点已重启 */
    uint16_t     sleep_s;               /* 节点深睡周期，0 = 常醒；睡着期间的命令等它醒来入网时补发 */
    uint16_t     lp_interval_ms;        /* 节点 ESP-NOW 省电：每隔多少毫秒开一次接收窗口，0 = 一直收 */
    uint8_t      lp_window_ms;          /* 接收窗口长度，SET 按半个窗口的间隔重发、覆盖一整个周期 */
    bool         stay_awake;            /* HB_ACK 里要求节点暂不深睡 */
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

/* 按 MAC 取一个节点，idx 可为 NULL；没有该节点返回 false */
bool gw_link_get_node(const uint8_t mac[6], gw_node_t *out, int *idx);

esp_err_t gw_link_set(int idx, uint8_t ch, uint16_t value);
esp_err_t gw_link_unpair(int idx);

/* 用户改名，name 为 1..MP_NAME_LEN 字节的 UTF-8；之后节点重新入网也不会被它上报的名字覆盖 */
esp_err_t gw_link_rename(int idx, const char *name);

/* 要求深睡节点保持唤醒（下次醒来的 HB_ACK 起生效），升级前用 */
void gw_link_stay_awake(const uint8_t mac[6], bool on);

/* 打开配对窗口 seconds 秒，0 立即关闭 */
void gw_link_pair(uint32_t seconds);
uint32_t gw_link_pair_remaining(void);

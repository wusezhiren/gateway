#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "meshproto.h"

/* 处理来自网关的 MP_OTA_* 帧（type 为 mp_parse 的结果），必要时回 ACK */
void node_ota_handle(int type, const uint8_t *data, int len, const uint8_t gw[6]);

/* 周期调用：长时间没有升级帧则放弃本次升级 */
void node_ota_poll(void);

/* 链路断开时放弃进行中的升级 */
void node_ota_cancel(void);

bool node_ota_active(void);

/* 比较 "x.y.z" 版本号，返回 <0 / 0 / >0 */
int node_ota_version_cmp(const char *a, const char *b);

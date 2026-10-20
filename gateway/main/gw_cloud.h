#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * 云端连接（MQTT over TLS，EMQX Cloud 等），协议见 docs/cloud.md。
 * 网关主动连 broker：发布保留的状态和在线标记，订阅命令；命令需带 API token。
 * 配置存 NVS（gw_cloud），通过 POST /api/cloud 或串口 cloud set 写入。
 */
typedef struct {
    bool configured;
    bool connected;
    char host[64];
    uint16_t port;
    char user[33];
    char error[32];     /* 最近一次连接失败原因，连上后清空 */
} gw_cloud_status_t;

esp_err_t gw_cloud_init(void);

/* host 为空表示关闭并清除配置 */
esp_err_t gw_cloud_config(const char *host, int port, const char *user, const char *pass);

void gw_cloud_get_status(gw_cloud_status_t *out);

/* 拷出 broker 密码（给已鉴权的 App 同步配置用） */
void gw_cloud_get_password(char *out, size_t len);

/* 网关 ID：STA MAC 小写不带冒号，topic 用 */
const char *gw_cloud_id(void);

/* 状态有变化，尽快上报（任意任务可调，不阻塞） */
void gw_cloud_notify(void);

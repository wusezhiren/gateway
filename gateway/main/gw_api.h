#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * App 用的局域网接口（节点、配对、绑定），挂在 gw_http 上，见 docs/api.md。
 * 绑定：App 请求后需在网关屏幕上点“允许”才下发 token。
 */
#define GW_API_NAME_LEN 24

typedef void (*gw_api_bind_cb_t)(void);

esp_err_t gw_api_register(httpd_handle_t httpd);

/* 有新的绑定请求时调用（在 httpd 任务里，不要阻塞） */
void gw_api_set_bind_cb(gw_api_bind_cb_t cb);

/* 当前是否有待确认的绑定请求，有则拷出手机名 */
bool gw_api_bind_pending(char name[GW_API_NAME_LEN + 1]);

/* 屏幕上的确认结果 */
void gw_api_bind_decide(bool allow);

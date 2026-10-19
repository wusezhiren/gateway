#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * 网关常驻 HTTP 服务（80 端口，STA 和配网热点两侧都可访问）。
 * 各模块在此之上注册自己的接口：配网接口只在配网期间注册。
 */
esp_err_t gw_http_start(void);
httpd_handle_t gw_http_handle(void);

/* 校验 "Authorization: Bearer <token>"；失败时已回复 401，调用方直接 return ESP_OK */
bool gw_http_authorized(httpd_req_t *req);

/* 回复 JSON 错误 {"error": msg} */
esp_err_t gw_http_error(httpd_req_t *req, const char *status, const char *msg);

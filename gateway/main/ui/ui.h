#pragma once

#include "gw_keys.h"

/* 网关屏幕 UI。ui_start() 之后，其他任务只能通过下面的函数和 UI 交互（内部只置标志，由 LVGL 任务处理） */
void ui_start(void);

/* 按键：任意键唤醒；VOL+/VOL- 切换页面 */
void ui_on_key(gw_key_t key, gw_key_event_t ev);

/* 已联网时长按 REC：弹出“重新配网”确认框 */
void ui_request_prov_confirm(void);

/* 有 App 请求绑定时调用（任意任务），屏幕弹出允许 / 拒绝 */
void ui_request_bind_confirm(void);

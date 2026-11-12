#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    S31_UI_PAGE_OVERVIEW = 0,
    S31_UI_PAGE_NODES,
    S31_UI_PAGE_FIRMWARE,
    S31_UI_PAGE_SETTINGS,
    S31_UI_PAGE_COUNT,
} s31_ui_page_t;

typedef struct {
    s31_ui_page_t page;
    bool wifi_connected;
    bool cloud_connected;
    uint16_t nodes_online;
    uint16_t nodes_total;
    bool screen_on;
    char status[64];
} s31_ui_model_t;

typedef void (*s31_ui_render_cb_t)(const s31_ui_model_t *model, void *context);

void s31_ui_model_reset(s31_ui_model_t *model);
esp_err_t s31_ui_set_page(s31_ui_model_t *model, int page);
esp_err_t s31_ui_next_page(s31_ui_model_t *model, int delta);
esp_err_t s31_ui_set_status(s31_ui_model_t *model, const char *status);
esp_err_t s31_ui_apply_message(s31_ui_model_t *model, uint32_t message_id,
                               const void *payload, size_t payload_len);
esp_err_t s31_ui_render(const s31_ui_model_t *model, s31_ui_render_cb_t callback, void *context);

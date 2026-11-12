#include "s31_ui.h"

#include <string.h>

void s31_ui_model_reset(s31_ui_model_t *model)
{
    if (!model) return;
    memset(model, 0, sizeof(*model));
    model->screen_on = true;
    model->page = S31_UI_PAGE_OVERVIEW;
}

esp_err_t s31_ui_set_page(s31_ui_model_t *model, int page)
{
    if (!model || page < 0 || page >= S31_UI_PAGE_COUNT) return ESP_ERR_INVALID_ARG;
    model->page = (s31_ui_page_t)page;
    return ESP_OK;
}

esp_err_t s31_ui_next_page(s31_ui_model_t *model, int delta)
{
    if (!model || (delta != 1 && delta != -1)) return ESP_ERR_INVALID_ARG;
    int page = (int)model->page + delta;
    if (page < 0) page = S31_UI_PAGE_COUNT - 1;
    if (page >= S31_UI_PAGE_COUNT) page = 0;
    model->page = (s31_ui_page_t)page;
    return ESP_OK;
}

esp_err_t s31_ui_set_status(s31_ui_model_t *model, const char *status)
{
    if (!model || !status) return ESP_ERR_INVALID_ARG;
    size_t n = 0;
    while (n < sizeof(model->status) && status[n]) ++n;
    if (n == sizeof(model->status)) return ESP_ERR_INVALID_SIZE;
    memcpy(model->status, status, n + 1);
    return ESP_OK;
}

esp_err_t s31_ui_apply_message(s31_ui_model_t *model, uint32_t message_id,
                               const void *payload, size_t payload_len)
{
    if (!model) return ESP_ERR_INVALID_ARG;
    if (message_id == 1) {
        if (!payload || payload_len != sizeof(bool)) return ESP_ERR_INVALID_SIZE;
        model->wifi_connected = *(const bool *)payload;
        return ESP_OK;
    }
    if (message_id == 2) {
        if (!payload || payload_len != sizeof(uint16_t) * 2) return ESP_ERR_INVALID_SIZE;
        const uint16_t *nodes = payload;
        if (nodes[0] > nodes[1]) return ESP_ERR_INVALID_ARG;
        model->nodes_online = nodes[0];
        model->nodes_total = nodes[1];
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t s31_ui_render(const s31_ui_model_t *model, s31_ui_render_cb_t callback, void *context)
{
    if (!model || !callback) return ESP_ERR_INVALID_ARG;
    callback(model, context);
    return ESP_OK;
}

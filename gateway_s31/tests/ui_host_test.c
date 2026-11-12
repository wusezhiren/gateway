#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "s31_ui.h"

static void render_cb(const s31_ui_model_t *model, void *context)
{
    *(int *)context = model->nodes_online;
}

int main(void)
{
    s31_ui_model_t model;
    bool wifi = true;
    uint16_t nodes[] = {2, 5};
    int rendered = 0;

    /* Positive: reset, wrap-around navigation, state updates and rendering. */
    s31_ui_model_reset(&model);
    assert(model.page == S31_UI_PAGE_OVERVIEW && model.screen_on);
    assert(s31_ui_next_page(&model, -1) == ESP_OK);
    assert(model.page == S31_UI_PAGE_SETTINGS);
    assert(s31_ui_next_page(&model, 1) == ESP_OK);
    assert(model.page == S31_UI_PAGE_OVERVIEW);
    assert(s31_ui_apply_message(&model, 1, &wifi, sizeof(wifi)) == ESP_OK);
    assert(model.wifi_connected);
    assert(s31_ui_apply_message(&model, 2, nodes, sizeof(nodes)) == ESP_OK);
    assert(model.nodes_online == 2 && model.nodes_total == 5);
    assert(s31_ui_set_status(&model, "ready") == ESP_OK);
    assert(strcmp(model.status, "ready") == 0);
    assert(s31_ui_render(&model, render_cb, &rendered) == ESP_OK);
    assert(rendered == 2);

    /* Negative: nulls, invalid page, invalid delta, oversized and unknown data. */
    assert(s31_ui_set_page(NULL, 0) == ESP_ERR_INVALID_ARG);
    assert(s31_ui_set_page(&model, -1) == ESP_ERR_INVALID_ARG);
    assert(s31_ui_set_page(&model, S31_UI_PAGE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(s31_ui_next_page(&model, 0) == ESP_ERR_INVALID_ARG);
    assert(s31_ui_set_status(&model, NULL) == ESP_ERR_INVALID_ARG);
    assert(s31_ui_set_status(&model, "12345678901234567890123456789012345678901234567890123456789012345") == ESP_ERR_INVALID_SIZE);
    assert(s31_ui_apply_message(&model, 1, NULL, sizeof(bool)) == ESP_ERR_INVALID_SIZE);
    assert(s31_ui_apply_message(&model, 2, nodes, sizeof(uint16_t)) == ESP_ERR_INVALID_SIZE);
    uint16_t invalid_nodes[] = {6, 5};
    assert(s31_ui_apply_message(&model, 2, invalid_nodes, sizeof(invalid_nodes)) == ESP_ERR_INVALID_ARG);
    assert(model.nodes_online == 2 && model.nodes_total == 5);
    assert(s31_ui_apply_message(&model, 99, NULL, 0) == ESP_ERR_NOT_SUPPORTED);
    assert(s31_ui_render(&model, NULL, NULL) == ESP_ERR_INVALID_ARG);

    puts("S31 UI host positive/negative tests passed");
    return 0;
}

#include "s31_display.h"

#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "s31_ui.h"
#include "s31_msgbus.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_touch.h"
#include "s31_screen_policy.h"
#include <string.h>

static s31_ui_model_t s_model;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_nodes;
static lv_display_t *s_display;
static esp_lcd_touch_handle_t s_touch;
static bool s_asleep;
static s31_screen_policy_t s_policy;

static const char *const s_pages[] = {"Overview", "Nodes", "Firmware", "Settings"};

static void update_ui(const s31_ui_model_t *model, void *context)
{
    (void)context;
    lv_label_set_text(s_title, s_pages[model->page]);
    lv_label_set_text(s_status, model->status);
    lv_label_set_text_fmt(s_nodes, "Nodes: %u/%u", model->nodes_online, model->nodes_total);
}

static void on_message(const s31_message_t *message, void *context)
{
    (void)context;
    if (message->id != S31_MSG_DISPLAY || message->length != sizeof(s31_ui_model_t) ||
        sizeof(s31_ui_model_t) > sizeof(message->data.bytes)) return;
    s31_ui_model_t next;
    memcpy(&next, message->data.bytes, sizeof(next));
    if (next.page >= S31_UI_PAGE_COUNT || next.nodes_online > next.nodes_total ||
        !memchr(next.status, 0, sizeof(next.status))) return;
    if (bsp_display_lock(100)) {
        s_model = next;
        s_model.screen_on = !s_asleep;
        if (!s_asleep) s31_ui_render(&s_model, update_ui, NULL);
        bsp_display_unlock();
    }
}

static void screen_task(void *arg)
{
    (void)arg;
    lv_indev_t *indev = bsp_display_get_input_dev();
    for (;;) {
        if (!s_asleep) {
            if (bsp_display_lock(100)) {
                uint32_t idle = lv_display_get_inactive_time(s_display);
                bool pressed = indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
                s31_screen_action_t action = s31_screen_policy_step(&s_policy, idle,
                    CONFIG_GW_S31_SCREEN_OFF_S * 1000U, pressed);
                bsp_display_unlock();
                if (action == S31_SCREEN_SLEEP) {
                    if (bsp_display_enter_sleep() == ESP_OK) {
                        s_asleep = true;
                        s_model.screen_on = false;
                        lvgl_port_stop();
                    } else {
                        s_policy.asleep = false;
                    }
                }
            }
        } else if (s_touch && esp_lcd_touch_read_data(s_touch) == ESP_OK) {
            esp_lcd_touch_point_data_t point;
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) == ESP_OK &&
                s31_screen_policy_step(&s_policy, 0, CONFIG_GW_S31_SCREEN_OFF_S * 1000U,
                                       count > 0) == S31_SCREEN_WAKE) {
                if (bsp_display_exit_sleep() == ESP_OK) {
                    s_asleep = false;
                    s_model.screen_on = true;
                    lvgl_port_resume();
                    if (bsp_display_lock(100)) {
                        lv_display_trigger_activity(s_display);
                        s31_ui_render(&s_model, update_ui, NULL);
                        if (indev) lv_indev_wait_release(indev);
                        bsp_display_unlock();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static const char *TAG = "s31_display";

esp_err_t s31_display_init(void)
{
    s31_ui_model_reset(&s_model);
    s31_ui_set_status(&s_model, "S31 gateway starting");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_GW_S31_LVGL_DRAW_LINES,
        .double_buffer = false,
        .flags = {.buff_spiram = true},
    };
    cfg.lvgl_port_cfg.task_affinity = 0; /* WakeNet will run on core 1. */
    cfg.lvgl_port_cfg.task_priority = CONFIG_GW_S31_LVGL_PRIORITY;
    cfg.lvgl_port_cfg.timer_period_ms = CONFIG_GW_S31_LVGL_TICK_MS;
    cfg.lvgl_port_cfg.task_max_sleep_ms = CONFIG_GW_S31_LVGL_MAX_SLEEP_MS;
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    if (!display) return ESP_ERR_NO_MEM;
    s_display = display;
    /* BSP touch input is owned by LVGL; use a separate touch handle only while asleep. */
    esp_err_t touch_err = bsp_touch_new(NULL, &s_touch);
    if (touch_err != ESP_OK) return touch_err;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101827), 0);
    s_title = lv_label_create(screen);
    if (!s_title) { bsp_display_unlock(); return ESP_ERR_NO_MEM; }
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 24);
    s_status = lv_label_create(screen);
    if (!s_status) { bsp_display_unlock(); return ESP_ERR_NO_MEM; }
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 0);
    s_nodes = lv_label_create(screen);
    if (!s_nodes) { bsp_display_unlock(); return ESP_ERR_NO_MEM; }
    lv_obj_set_style_text_color(s_nodes, lv_color_white(), 0);
    lv_obj_align(s_nodes, LV_ALIGN_BOTTOM_MID, 0, -24);
    s31_ui_render(&s_model, update_ui, NULL);
    bsp_display_unlock();
    esp_err_t err = s31_msgbus_subscribe(S31_MSG_DISPLAY, on_message, NULL);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(screen_task, "s31_screen", 3072, NULL, 2, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "RGB LCD 800x480 and backlight ready");
    return ESP_OK;
}

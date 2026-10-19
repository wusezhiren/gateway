#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_keys.h"

static const char *TAG = "gw_keys";

#define KEY_ADC_UNIT    ADC_UNIT_1
#define KEY_ADC_CH      ADC_CHANNEL_4       /* GPIO5 */
#define POLL_MS         20
#define DEBOUNCE_CNT    3                   /* 连续 3 次一致才认（60ms） */
#define IDLE_MAX_MV     2700                /* 高于此值视为无按键 */

/* 各键分压中心（mV），取相邻中点作判定边界 */
static const int s_center_mv[GW_KEY_COUNT] = {380, 820, 1180, 1570, 1980, 2410};

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static gw_key_cb_t s_cb;
static volatile bool s_debug;

static int mv_to_key(int mv)
{
    if (mv < s_center_mv[0] / 2 || mv > IDLE_MAX_MV) {
        return -1;
    }
    for (int i = 0; i < GW_KEY_COUNT - 1; i++) {
        if (mv < (s_center_mv[i] + s_center_mv[i + 1]) / 2) {
            return i;
        }
    }
    return GW_KEY_COUNT - 1;
}

static int read_mv(void)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, KEY_ADC_CH, &raw) != ESP_OK) {
        return -1;
    }
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return raw * 3100 / 4095;   /* 无校准时粗略换算 */
}

static void emit(int key, gw_key_event_t ev)
{
    static const char *const names[] = {"press", "click", "long"};
    ESP_LOGI(TAG, "key %d %s", key, names[ev]);
    if (s_cb) {
        s_cb((gw_key_t)key, ev);
    }
}

static void keys_task(void *arg)
{
    int stable = -1, candidate = -1, same = 0;
    int held_ms = 0;
    bool long_fired = false;
    int last_dbg_mv = -1000;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int mv = read_mv();
        int key = mv_to_key(mv);

        if (s_debug && (mv - last_dbg_mv > 50 || last_dbg_mv - mv > 50)) {
            ESP_LOGI(TAG, "adc %d mV -> key %d", mv, key);
            last_dbg_mv = mv;
        }

        if (key == candidate) {
            same++;
        } else {
            candidate = key;
            same = 1;
        }
        if (same < DEBOUNCE_CNT) {
            if (stable >= 0) {
                held_ms += POLL_MS;
            }
        } else if (candidate != stable) {
            /* 松开或换键：上一个键若未触发长按则算短按 */
            if (stable >= 0 && !long_fired) {
                emit(stable, GW_KEY_EV_CLICK);
            }
            stable = candidate;
            held_ms = 0;
            long_fired = false;
            if (stable >= 0) {
                emit(stable, GW_KEY_EV_PRESS);
            }
        } else if (stable >= 0) {
            held_ms += POLL_MS;
        }

        if (stable >= 0 && !long_fired && held_ms >= GW_KEY_LONG_MS) {
            long_fired = true;
            emit(stable, GW_KEY_EV_LONG);
        }
    }
}

esp_err_t gw_keys_start(gw_key_cb_t cb)
{
    s_cb = cb;
    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = KEY_ADC_UNIT};
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }
    adc_oneshot_chan_cfg_t ch_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    err = adc_oneshot_config_channel(s_adc, KEY_ADC_CH, &ch_cfg);
    if (err != ESP_OK) {
        return err;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = KEY_ADC_UNIT,
        .chan = KEY_ADC_CH,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable, using raw estimate");
        s_cali = NULL;
    }

    if (xTaskCreate(keys_task, "gw_keys", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gw_keys_set_debug(bool on)
{
    s_debug = on;
}

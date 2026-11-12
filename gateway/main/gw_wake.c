#include "gw_wake.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "model_path.h"
#include "esp_wn_models.h"
#include "gw_sd.h"
#include "gw_audio.h"
#include "gw_voice_ws.h"
#include "gw_cloud.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gw_wake";
static const char *MODEL = "wn9s_nihaoxiaozhi";
static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_iface;
static model_iface_data_t *s_data;
static TaskHandle_t s_task;
static int16_t *s_samples;
static size_t s_used;
static int s_chunk;
static volatile bool s_detected;
static volatile bool s_pause;
static volatile bool s_idle = true;
static volatile uint32_t s_chunks;
static volatile uint32_t s_captured;
static volatile uint32_t s_wake_count;
static volatile uint16_t s_peak;
static volatile uint16_t s_max_peak;
static volatile uint32_t s_detect_max_us;
static volatile uint64_t s_detect_total_us;
static volatile int s_test_result = -1;
static SemaphoreHandle_t s_model_lock;
static volatile bool s_file_testing;
static volatile bool s_voice_active;
typedef struct { int16_t samples[512]; } wake_frame_t;
static QueueHandle_t s_audio_q;
static volatile uint32_t s_queue_overruns;
/* Last 2 seconds of mic PCM, retained across wake detection. */
#define WAKE_PREROLL_SAMPLES (16000 * 4)
static int16_t *s_preroll;
static size_t s_preroll_pos;
static size_t s_preroll_count;
static volatile bool s_handoff_ready;
static size_t s_wake_pos;
static volatile bool s_barge_running;
static volatile bool s_barge_detected;
static volatile bool s_barge_done = true;
static volatile bool s_barge_ready;
static int16_t *s_barge_samples;
static size_t s_barge_used;
static int16_t *s_barge_preroll;
static size_t s_barge_pos;
static size_t s_barge_count;
static size_t s_barge_after_wake;
static volatile bool s_barge_handoff;

static esp_err_t barge_feed(const int16_t *samples, size_t count, void *arg)
{
    if (!s_barge_running) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count; ++i) {
        int16_t sample = samples[i];
        s_barge_preroll[s_barge_pos] = sample;
        s_barge_pos = (s_barge_pos + 1) % WAKE_PREROLL_SAMPLES;
        if (s_barge_count < WAKE_PREROLL_SAMPLES) s_barge_count++;
        if (s_barge_detected) {
            if (s_barge_after_wake < WAKE_PREROLL_SAMPLES) s_barge_after_wake++;
            continue; /* retain command audio until old session closes */
        }
        int32_t boosted = (int32_t)sample * 2;
        if (boosted > INT16_MAX) boosted = INT16_MAX;
        if (boosted < INT16_MIN) boosted = INT16_MIN;
        s_barge_samples[s_barge_used++] = (int16_t)boosted;
        if (s_barge_used == (size_t)s_chunk) {
            s_barge_used = 0;
            xSemaphoreTake(s_model_lock, portMAX_DELAY);
            wakenet_state_t result = s_iface->detect(s_data, s_barge_samples);
            xSemaphoreGive(s_model_lock);
            if (result == WAKENET_DETECTED) {
                s_barge_detected = true;
                s_barge_after_wake = 0;
                ESP_LOGI(TAG, "barge-in wake word detected");
            }
        }
    }
    return s_barge_running ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void barge_task(void *arg)
{
    s_barge_samples = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_barge_preroll = heap_caps_malloc(WAKE_PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_barge_samples && s_barge_preroll) {
        esp_err_t err = gw_audio_capture_duplex(12000, barge_feed, NULL);
        ESP_LOGI(TAG, "barge-in capture ended: %s detected=%d", esp_err_to_name(err), s_barge_detected);
    }
    free(s_barge_samples); s_barge_samples = NULL;
    s_barge_ready = false;
    /* Keep preroll allocated until the waiting wake task takes ownership. */
    s_barge_done = true;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t gw_wake_begin_barge_in(void)
{
    if (s_barge_running || !s_barge_done || !s_data || !s_pause) return ESP_ERR_INVALID_STATE;
    s_barge_done = false;
    s_barge_ready = false;
    s_barge_detected = false;
    s_barge_used = 0;
    s_barge_pos = s_barge_count = 0;
    s_barge_after_wake = 0;
    free(s_barge_preroll); s_barge_preroll = NULL;
    s_barge_handoff = false;
    s_barge_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(barge_task, "barge_mic", 8192, NULL, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_barge_running = false;
        s_barge_done = true;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool gw_wake_barge_in_detected(void) { return s_barge_detected; }
bool gw_wake_barge_in_ready(void) { return s_barge_ready; }
void gw_wake_barge_in_channels_ready(void) { s_barge_ready = true; }

bool gw_wake_take_barge_in(void)
{
    if (!s_barge_handoff || !s_barge_preroll) return false;
    size_t elapsed = s_barge_after_wake;
    size_t saved = elapsed + 12800;
    if (saved > s_barge_count) saved = s_barge_count;
    if (saved > WAKE_PREROLL_SAMPLES) saved = WAKE_PREROLL_SAMPLES;
    size_t start = (s_barge_pos + WAKE_PREROLL_SAMPLES - saved) % WAKE_PREROLL_SAMPLES;
    for (size_t i = 0; i < saved; ++i)
        s_preroll[i] = s_barge_preroll[(start + i) % WAKE_PREROLL_SAMPLES];
    s_preroll_pos = saved % WAKE_PREROLL_SAMPLES;
    s_preroll_count = saved;
    /* The handoff only needs the final saved range; retain at most one ring. */
    /* If the command filled the ring, the wake-word tail has been overwritten.
     * Keep all available command audio rather than synthesizing old samples. */
    size_t available_after_wake = elapsed < saved ? elapsed : saved;
    s_wake_pos = (s_preroll_pos + WAKE_PREROLL_SAMPLES - available_after_wake) % WAKE_PREROLL_SAMPLES;
    s_handoff_ready = true;
    s_barge_handoff = false;
    free(s_barge_preroll); s_barge_preroll = NULL;
    return true;
}

void gw_wake_discard_barge_in(void)
{
    s_barge_handoff = false;
    s_barge_detected = false;
    free(s_barge_preroll);
    s_barge_preroll = NULL;
}

void gw_wake_end_barge_in(void)
{
    s_barge_running = false;
    while (!s_barge_done) vTaskDelay(pdMS_TO_TICKS(20));
    s_barge_handoff = s_barge_detected;
    if (!s_barge_handoff) { free(s_barge_preroll); s_barge_preroll = NULL; }
}

static void wake_release_for_voice(void)
{
    if (s_audio_q) { vQueueDeleteWithCaps(s_audio_q); s_audio_q = NULL; }
    if (s_samples) { free(s_samples); s_samples = NULL; }
}

static esp_err_t wake_restore_after_voice(void)
{
    if (!s_samples) s_samples = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_audio_q) s_audio_q = xQueueCreateWithCaps(16, sizeof(wake_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_samples && s_audio_q ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gw_wake_suspend_for_voice(void)
{
    gw_wake_pause();
    wake_release_for_voice();
    return ESP_OK;
}

esp_err_t gw_wake_resume_after_voice(void)
{
    esp_err_t err = wake_restore_after_voice();
    if (err == ESP_OK && s_audio_q) xQueueReset(s_audio_q);
    s_voice_active = false;
    gw_wake_resume();
    return err;
}

typedef struct { uint8_t *pcm; size_t bytes; } wake_test_t;
static void wake_test_task(void *arg)
{
    wake_test_t *test = arg;
    s_file_testing = true;
    gw_wake_pause();
    while (!s_idle) vTaskDelay(pdMS_TO_TICKS(20));
    if (!s_data && gw_wake_init() != ESP_OK) {
        ESP_LOGE(TAG, "file test: model unavailable");
        s_test_result = -1;
    } else {
        model_iface_data_t *test_model = s_iface->create(MODEL, DET_MODE_90);
        int16_t *test_samples = malloc(s_chunk * sizeof(int16_t));
        int hits = 0;
        const int16_t *samples = (const int16_t *)test->pcm;
        size_t count = test->bytes / sizeof(int16_t);
        size_t used = 0;
        size_t wake_offset = 0;
        if (!test_model || !test_samples) {
            ESP_LOGE(TAG, "file test: allocation failed");
            s_test_result = -1;
        } else for (size_t off = 0; off < count; ++off) {
            test_samples[used++] = samples[off];
            if (used == (size_t)s_chunk) {
                used = 0;
                wakenet_state_t state = s_iface->detect(test_model, test_samples);
                if (state == WAKENET_DETECTED) {
                    ++hits;
                    if (!wake_offset) wake_offset = off + 1;
                    ESP_LOGI(TAG, "file test: 你好小智 detected at %.2f s", (double)(off + 1) / 16000.0);
                }
            }
        }
        if (test_model && test_samples) s_test_result = hits;
        free(test_samples);
        if (test_model) s_iface->destroy(test_model);
        ESP_LOGI(TAG, "file test: %u samples, detections=%d", (unsigned)count, hits);
        if (hits && wake_offset < count) {
            gw_cloud_pause();
            for (int i = 0; i < 30 && !gw_cloud_is_paused(); ++i) vTaskDelay(pdMS_TO_TICKS(20));
            esp_err_t reply_err = gw_sd_play_wake_reply();
            ESP_LOGI(TAG, "file test: local reply %s", esp_err_to_name(reply_err));
            size_t pre_roll = 12800;
            size_t send_offset = wake_offset > pre_roll ? wake_offset - pre_roll : 0;
            esp_err_t voice_err = gw_voice_ws_probe_barge_pcm(samples + send_offset, count - send_offset);
            ESP_LOGI(TAG, "file test: cloud session %s", esp_err_to_name(voice_err));
            bool interrupted = gw_wake_barge_in_detected();
            if (interrupted) {
                ESP_LOGI(TAG, "file test: interrupted handoff ready, using live buffered command");
                voice_err = gw_voice_ws_listen_with_preroll(NULL, 0);
                ESP_LOGI(TAG, "file test: second cloud session %s", esp_err_to_name(voice_err));
            }
            gw_wake_end_barge_in();
            gw_wake_discard_barge_in();
            gw_cloud_resume();
            ESP_LOGI(TAG, "file test: local wake/reply/cloud complete");
        }
    }
    gw_wake_resume();
    s_file_testing = false;
    free(test->pcm);
    free(test);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t gw_wake_test_pcm(const uint8_t *pcm, size_t bytes)
{
    if (!pcm || !bytes || bytes % sizeof(int16_t) || bytes > 320000 ||
        s_test_result == -2 || s_file_testing) return ESP_ERR_INVALID_ARG;
    wake_test_t *test = malloc(sizeof(*test));
    if (!test) return ESP_ERR_NO_MEM;
    test->pcm = malloc(bytes);
    if (!test->pcm) { free(test); return ESP_ERR_NO_MEM; }
    memcpy(test->pcm, pcm, bytes);
    test->bytes = bytes;
    s_test_result = -2;
    if (xTaskCreateWithCaps(wake_test_task, "wake_file", 16384, test, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_test_result = -1;
        free(test->pcm); free(test);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t wake_feed(const int16_t *samples, size_t count, void *arg)
{
    if (s_pause) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count; ++i) {
        s_preroll[s_preroll_pos] = samples[i];
        s_preroll_pos = (s_preroll_pos + 1) % WAKE_PREROLL_SAMPLES;
        if (s_preroll_count < WAKE_PREROLL_SAMPLES) s_preroll_count++;
    }
    if (s_voice_active) return ESP_OK; /* keep buffering during WebSocket setup */
    while (count > 0) {
        size_t n = s_chunk - s_used;
        if (n > count) n = count;
        for (size_t i = 0; i < n; ++i) {
            /* The Korvo mic path is quiet relative to the WakeNet reference
             * audio. Apply a bounded 2x gain; the limiter prevents clipping. */
            int32_t boosted = (int32_t)samples[i] * 2;
            if (boosted > INT16_MAX) boosted = INT16_MAX;
            if (boosted < INT16_MIN) boosted = INT16_MIN;
            s_samples[s_used + i] = (int16_t)boosted;
        }
        samples += n;
        count -= n;
        s_used += n;
        if (s_used == (size_t)s_chunk) {
            s_used = 0;
            uint16_t peak = 0;
            for (int i = 0; i < s_chunk; ++i) {
                int32_t value = s_samples[i];
                uint16_t magnitude = value < 0 ? -value : value;
                if (magnitude > peak) peak = magnitude;
            }
            s_peak = peak;
            if (peak > s_max_peak) s_max_peak = peak;
            s_captured++;
            wake_frame_t frame;
            memcpy(frame.samples, s_samples, sizeof(frame.samples));
            if (xQueueSend(s_audio_q, &frame, 0) != pdTRUE) {
                s_queue_overruns++;
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_OK;
}

static void wake_task(void *arg)
{
    for (;;) {
        wake_frame_t frame;
        if (!s_audio_q || xQueueReceive(s_audio_q, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (s_pause || s_voice_active) continue;
        xSemaphoreTake(s_model_lock, portMAX_DELAY);
        int64_t started = esp_timer_get_time();
        wakenet_state_t state = s_iface->detect(s_data, frame.samples);
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        s_detect_total_us += elapsed;
        if (elapsed > s_detect_max_us) s_detect_max_us = elapsed;
        xSemaphoreGive(s_model_lock);
        s_chunks++;
        if (state == WAKENET_DETECTED) {
            s_wake_count++;
            s_detected = true;
            s_voice_active = true;
            xQueueReset(s_audio_q);
            s_wake_pos = s_preroll_pos;
            s_handoff_ready = true;
            ESP_LOGI(TAG, "你好小智 detected; preparing voice session");
            gw_cloud_pause();
            for (int i = 0; i < 30 && !gw_cloud_is_paused(); ++i) vTaskDelay(pdMS_TO_TICKS(20));
            bool interrupted;
            do {
                esp_err_t voice_err = gw_voice_ws_listen_with_preroll(NULL, 0);
                ESP_LOGI(TAG, "voice session ended: %s", esp_err_to_name(voice_err));
                interrupted = voice_err == ESP_OK && gw_wake_barge_in_detected();
                if (!interrupted) {
                    gw_wake_end_barge_in();
                    gw_wake_discard_barge_in();
                }
                if (interrupted) ESP_LOGI(TAG, "starting interrupted command");
            } while (interrupted);
            gw_cloud_resume();
            /* WakeNet9s clean() crashes after a session; discard partial input instead. */
            s_used = 0;
            s_preroll_count = 0;
            s_handoff_ready = false;
            xQueueReset(s_audio_q);
            s_voice_active = false;
            gw_wake_resume();
            ESP_LOGI(TAG, "wake detection resumed");
        }
    }
}

size_t gw_wake_take_handoff(int16_t *out, size_t capacity)
{
    if (s_barge_detected && s_barge_preroll) {
        gw_wake_end_barge_in();
        if (!gw_wake_take_barge_in()) return 0;
    }
    if (!out || !s_handoff_ready) return 0;
    gw_wake_pause(); /* capture closes I2S before the voice task opens it */
    if (!s_idle) {
        ESP_LOGE(TAG, "handoff failed: capture still owns I2S");
        return 0;
    }
    size_t elapsed = (s_preroll_pos + WAKE_PREROLL_SAMPLES - s_wake_pos) % WAKE_PREROLL_SAMPLES;
    size_t pre_roll = 12800; /* 0.8 s of wake-word tail */
    size_t saved = elapsed + pre_roll;
    if (saved > s_preroll_count) saved = s_preroll_count;
    if (saved > capacity) saved = capacity;
    size_t start = (s_preroll_pos + WAKE_PREROLL_SAMPLES - saved) % WAKE_PREROLL_SAMPLES;
    for (size_t i = 0; i < saved; ++i) out[i] = s_preroll[(start + i) % WAKE_PREROLL_SAMPLES];
    s_handoff_ready = false;
    return saved;
}

static void capture_task(void *arg)
{
    for (;;) {
        if (s_pause) { s_idle = true; vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        s_idle = false;
        esp_err_t err = gw_audio_capture_quiet(30000, wake_feed, NULL);
        s_idle = true;
        if (err != ESP_OK && !s_pause) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void gw_wake_pause(void)
{
    s_pause = true;
    for (int i = 0; i < 50 && !s_idle; ++i) vTaskDelay(pdMS_TO_TICKS(20));
}

void gw_wake_resume(void) { s_pause = false; }

esp_err_t gw_wake_init(void)
{
    if (s_data) return ESP_OK;
    if (!gw_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    s_models = esp_srmodel_init("/sdcard/srmodels");
    if (!s_models || esp_srmodel_exists(s_models, (char *)MODEL) < 0) {
        ESP_LOGW(TAG, "model %s not found on SD card", MODEL);
        return ESP_ERR_NOT_FOUND;
    }
    s_iface = esp_wn_handle_from_name(MODEL);
    if (!s_iface) return ESP_ERR_NOT_SUPPORTED;
    s_data = s_iface->create(MODEL, DET_MODE_90);
    if (!s_data) {
        ESP_LOGE(TAG, "model create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "model ready: %s rate=%d chunk=%d channels=%d internal=%u psram=%u",
             MODEL, s_iface->get_samp_rate(s_data), s_iface->get_samp_chunksize(s_data),
             s_iface->get_channel_num(s_data),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_chunk = s_iface->get_samp_chunksize(s_data);
    if (s_iface->get_samp_rate(s_data) != 16000 || s_iface->get_channel_num(s_data) != 1 ||
        s_chunk <= 0 || s_chunk > 4096) {
        s_iface->destroy(s_data);
        s_data = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_samples = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_samples) {
        s_iface->destroy(s_data);
        s_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_model_lock = xSemaphoreCreateMutex();
    if (!s_model_lock) return ESP_ERR_NO_MEM;
    s_audio_q = xQueueCreateWithCaps(16, sizeof(wake_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_audio_q) return ESP_ERR_NO_MEM;
    s_preroll = heap_caps_malloc(WAKE_PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_preroll) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t gw_wake_start(void)
{
    esp_err_t err = gw_wake_init();
    if (err != ESP_OK || s_task) return err;
    /* Keep WakeNet inference off CPU0, which hosts Wi-Fi/ESP-NOW and LVGL. */
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "wake_infer", 32768, NULL, 4,
                                        &s_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    TaskHandle_t capture = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(capture_task, "wake_capture", 16384, NULL, 5,
                                        &capture, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void gw_wake_status(void)
{
    printf("wake: %s model=%s sd=%s paused=%d idle=%d captured=%lu inferred=%lu peak=%u max_peak=%u detections=%lu overruns=%lu detect_avg_us=%llu detect_max_us=%lu\n",
           s_task ? "listening" : s_data ? "ready" : "not_ready", MODEL,
           gw_sd_is_mounted() ? "mounted" : "missing",
           (int)s_pause, (int)s_idle, (unsigned long)s_captured, (unsigned long)s_chunks,
           (unsigned)s_peak, (unsigned)s_max_peak,
           (unsigned long)s_wake_count, (unsigned long)s_queue_overruns,
           (unsigned long long)(s_chunks ? s_detect_total_us / s_chunks : 0),
           (unsigned long)s_detect_max_us);
    if (s_data) printf("rate=%d chunk=%d channels=%d\n", s_iface->get_samp_rate(s_data),
                       s_iface->get_samp_chunksize(s_data), s_iface->get_channel_num(s_data));
    printf("file_test_detections=%d\n", (int)s_test_result);
}

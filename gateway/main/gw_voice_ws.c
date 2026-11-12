#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "gw_voice_tools.h"
#include "gw_voice_ws.h"
#include "gw_cloud.h"
#include "gw_audio.h"
#include "gw_wake.h"
#include "tts_resample.h"
#include "voice_vad.h"
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_audio_enc_reg.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"
#include "esp_opus_dec.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"

static const char *TAG = "gw_voice_ws";
#define NS "gw_voice"
static char s_url[256];
static char s_token[192];
static char s_headers[512];
static char s_client_id[37];
static char s_session_id[96];
static bool s_hello_ok;
static volatile bool s_mcp_ready;
static volatile bool s_tts_done;
static char s_activation_challenge[192];
static char s_activation_url[256] = "https://api.tenclass.net/xiaozhi/ota/";
static char s_pending_ws_url[256];
static char s_pending_ws_token[192];
static char *s_http_body;
static size_t s_http_body_len;

static esp_err_t activation_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_http_body && e->data_len > 0) {
        size_t room = 16383 - s_http_body_len;
        size_t n = e->data_len < room ? e->data_len : room;
        memcpy(s_http_body + s_http_body_len, e->data, n);
        s_http_body_len += n;
        s_http_body[s_http_body_len] = 0;
    }
    return ESP_OK;
}
static esp_websocket_client_handle_t s_client;
static esp_audio_dec_handle_t s_tts_dec;
static esp_codec_dev_handle_t s_tts_codec;
static const audio_codec_if_t *s_tts_codec_if;
static const audio_codec_data_if_t *s_tts_data;
static const audio_codec_ctrl_if_t *s_tts_ctrl;
static const audio_codec_gpio_if_t *s_tts_gpio;
static i2s_chan_handle_t s_tts_tx;
static bool s_tts_tx_borrowed;
static uint8_t *s_tts_pcm;
static int16_t *s_tts_stereo;
static tts_resample_t s_tts_resample;
typedef struct { uint16_t len; uint8_t data[1024]; } tts_packet_t;
static QueueHandle_t s_tts_queue;
static TaskHandle_t s_tts_task;
static SemaphoreHandle_t s_tts_finished;
static volatile bool s_tts_end_requested;
static volatile bool s_tts_cancelled;
static void tts_close(void);
static void tts_binary(const uint8_t *p, size_t len);

static void tts_task(void *arg)
{
    tts_packet_t packet;
    while (!s_tts_cancelled && (!s_tts_end_requested || (s_tts_queue && uxQueueMessagesWaiting(s_tts_queue)))) {
        if (xQueueReceive(s_tts_queue, &packet, pdMS_TO_TICKS(100)) == pdTRUE)
            tts_binary(packet.data, packet.len);
    }
    tts_close();
    s_tts_done = true;
    if (s_tts_finished) xSemaphoreGive(s_tts_finished);
    s_tts_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static bool tts_start_worker(void)
{
    if (s_tts_task) return true;
    if (!s_tts_queue) s_tts_queue = xQueueCreateWithCaps(24, sizeof(tts_packet_t),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tts_queue) return false;
    if (!s_tts_finished) s_tts_finished = xSemaphoreCreateBinary();
    if (!s_tts_finished) return false;
    xSemaphoreTake(s_tts_finished, 0);
    s_tts_end_requested = false;
    s_tts_cancelled = false;
    return xTaskCreatePinnedToCoreWithCaps(tts_task, "tts_play", 24576, NULL, 4,
                                            &s_tts_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

static void tts_close(void)
{
    if (s_tts_dec) { esp_audio_dec_close(s_tts_dec); s_tts_dec = NULL; esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS); }
    if (s_tts_codec) { esp_codec_dev_close(s_tts_codec); esp_codec_dev_delete(s_tts_codec); s_tts_codec = NULL; }
    if (s_tts_codec_if) { audio_codec_delete_codec_if(s_tts_codec_if); s_tts_codec_if = NULL; }
    if (s_tts_gpio) { audio_codec_delete_gpio_if(s_tts_gpio); s_tts_gpio = NULL; }
    if (s_tts_data) { audio_codec_delete_data_if(s_tts_data); s_tts_data = NULL; }
    if (s_tts_ctrl) { audio_codec_delete_ctrl_if(s_tts_ctrl); s_tts_ctrl = NULL; }
    if (s_tts_tx) { if (!s_tts_tx_borrowed) i2s_del_channel(s_tts_tx); s_tts_tx = NULL; s_tts_tx_borrowed = false; }
    free(s_tts_pcm); s_tts_pcm = NULL;
    free(s_tts_stereo); s_tts_stereo = NULL;
    s_tts_resample = (tts_resample_t){0};
}

static esp_err_t tts_open(void)
{
    if (s_tts_dec) return ESP_OK;
    esp_opus_dec_cfg_t dc = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dc.sample_rate = 24000; dc.channel = 1; dc.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dc.self_delimited = false;
    if (esp_opus_dec_register() != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    esp_audio_dec_cfg_t ac = {.type=ESP_AUDIO_TYPE_OPUS, .cfg=&dc, .cfg_sz=sizeof(dc)};
    if (esp_audio_dec_open(&ac, &s_tts_dec) != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    i2c_master_bus_handle_t bus; if (i2c_master_get_bus_handle(I2C_NUM_1, &bus) != ESP_OK) return ESP_FAIL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num=2; cc.dma_frame_num=64;
    s_tts_tx = gw_audio_duplex_tx();
    s_tts_tx_borrowed = s_tts_tx != NULL;
    if (s_tts_tx_borrowed && !gw_wake_barge_in_ready()) return ESP_ERR_INVALID_STATE;
    if (!s_tts_tx_borrowed && i2s_new_channel(&cc, &s_tts_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "TTS TX unavailable");
        return ESP_FAIL;
    }
    i2s_std_config_t sc = {.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=GPIO_NUM_16,.bclk=GPIO_NUM_9,.ws=GPIO_NUM_45,.dout=GPIO_NUM_8,.din=I2S_GPIO_UNUSED}};
    sc.clk_cfg.mclk_multiple=I2S_MCLK_MULTIPLE_256;
    if (!s_tts_tx_borrowed && i2s_channel_init_std_mode(s_tts_tx,&sc)!=ESP_OK) {
        ESP_LOGE(TAG, "TTS TX initialization failed");
        return ESP_FAIL;
    }
    audio_codec_i2c_cfg_t ic={.port=I2C_NUM_1,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=bus};
    s_tts_ctrl=audio_codec_new_i2c_ctrl(&ic); audio_codec_i2s_cfg_t id={.port=I2S_NUM_0,.tx_handle=s_tts_tx};
    s_tts_data=audio_codec_new_i2s_data(&id); s_tts_gpio=audio_codec_new_gpio();
    es8311_codec_cfg_t ec={.ctrl_if=s_tts_ctrl,.gpio_if=s_tts_gpio,.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.master_mode=false,.use_mclk=true,.pa_pin=GPIO_NUM_48,.mclk_div=256,.hw_gain={.pa_voltage=5.0f,.codec_dac_voltage=3.3f}};
    s_tts_codec_if=es8311_codec_new(&ec); esp_codec_dev_cfg_t ed={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=s_tts_codec_if,.data_if=s_tts_data};
    s_tts_codec=esp_codec_dev_new(&ed); esp_codec_dev_sample_info_t si={.bits_per_sample=16,.channel=2,.channel_mask=3,.sample_rate=16000};
    if (!s_tts_codec || esp_codec_dev_open(s_tts_codec,&si)!=ESP_CODEC_DEV_OK || esp_codec_dev_set_out_vol(s_tts_codec,55)!=ESP_CODEC_DEV_OK) return ESP_FAIL;
    return ESP_OK;
}

static void tts_binary(const uint8_t *p, size_t len)
{
    if (!len || tts_open()!=ESP_OK) return;
    if (!s_tts_pcm) s_tts_pcm = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tts_stereo) s_tts_stereo = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tts_pcm || !s_tts_stereo) { ESP_LOGE(TAG, "TTS buffer allocation failed"); return; }
    esp_audio_dec_in_raw_t raw={.buffer=(uint8_t *)p,.len=len};
    esp_audio_dec_out_frame_t out={.buffer=s_tts_pcm,.len=4096};
    if (esp_audio_dec_process(s_tts_dec,&raw,&out)==ESP_AUDIO_ERR_OK) {
        size_t samples=out.decoded_size/sizeof(int16_t);
        size_t produced=tts_resample_24_to_16(&s_tts_resample,(const int16_t *)s_tts_pcm,
                                               samples,s_tts_stereo,8192/(2*sizeof(int16_t)));
        if (produced && !s_tts_cancelled) esp_codec_dev_write(s_tts_codec,s_tts_stereo,produced*2*sizeof(int16_t));
    }
}

typedef struct {
    esp_audio_enc_handle_t enc;
    int16_t *pcm;
    uint8_t *opus;
    size_t used;
    int frame_samples;
    int packet_capacity;
    esp_err_t err;
    voice_vad_t vad;
    QueueHandle_t pcm_queue;
    volatile bool capture_done;
    volatile bool stop_requested;
    volatile uint32_t dropped_frames;
    int64_t capture_started_us;
    int64_t capture_finished_us;
    size_t sent_samples;
    size_t sent_frames;
    size_t sent_bytes;
    unsigned capture_limit_ms;
    size_t live_samples;
} listen_ctx_t;

typedef struct { int16_t samples[512]; uint16_t count; } voice_pcm_frame_t;

static esp_err_t encode_pcm(const int16_t *samples, size_t count, listen_ctx_t *x)
{
    while (count && x->err == ESP_OK) {
        size_t n = x->frame_samples - x->used;
        if (n > count) n = count;
        memcpy(x->pcm + x->used, samples, n * sizeof(int16_t));
        x->used += n; samples += n; count -= n;
        if (x->used != (size_t)x->frame_samples) continue;
        esp_audio_enc_in_frame_t in = {.buffer=(uint8_t *)x->pcm,
                                       .len=(size_t)x->frame_samples * 2};
        esp_audio_enc_out_frame_t out = {.buffer=x->opus, .len=x->packet_capacity};
        if (esp_audio_enc_process(x->enc, &in, &out) != ESP_AUDIO_ERR_OK || !out.encoded_bytes) {
            x->err = ESP_FAIL; break;
        }
        /* Protocol-Version 1 sends each Opus packet as one raw WS binary
         * frame. BinaryProtocol3 headers belong only to protocol version 3. */
        int sent = esp_websocket_client_send_bin(s_client, (const char *)x->opus,
                                                 out.encoded_bytes, pdMS_TO_TICKS(3000));
        if (sent != (int)out.encoded_bytes) {
            x->err = ESP_FAIL; break;
        }
        x->sent_samples += x->frame_samples;
        x->sent_frames++;
        x->sent_bytes += sent;
        x->used = 0;
    }
    return x->err;
}

static esp_err_t listen_pcm(const int16_t *samples, size_t count, void *arg)
{
    listen_ctx_t *x = arg;
    voice_vad_observe(&x->vad, samples, count);
    x->live_samples += count;
    if (x->pcm_queue) {
        while (count) {
            voice_pcm_frame_t frame = {.count = count > 512 ? 512 : count};
            memcpy(frame.samples, samples, frame.count * sizeof(int16_t));
            if (xQueueSend(x->pcm_queue, &frame, 0) != pdTRUE) {
                x->dropped_frames++;
                return ESP_ERR_NO_MEM;
            }
            samples += frame.count;
            count -= frame.count;
        }
        if (voice_vad_should_stop_live(&x->vad, x->live_samples)) {
            x->stop_requested = true;
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    encode_pcm(samples, count, x);
    if (x->err == ESP_OK && voice_vad_should_stop(&x->vad, false)) return ESP_ERR_INVALID_STATE;
    return x->err;
}

static void voice_capture_task(void *arg)
{
    listen_ctx_t *x = arg;
    x->capture_started_us = esp_timer_get_time();
    esp_err_t result = gw_audio_capture_quiet(x->capture_limit_ms, listen_pcm, x);
    x->capture_finished_us = esp_timer_get_time();
    if (result == ESP_ERR_INVALID_STATE && x->stop_requested) result = ESP_OK;
    if (result != ESP_OK) x->err = result;
    x->capture_done = true;
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t gw_voice_ws_listen_impl(const int16_t *file_pcm, size_t file_samples, bool auto_mode, bool continue_mic,
                                        bool probe_barge)
{
    int64_t session_started_us = esp_timer_get_time();
    s_tts_done = false;
    s_tts_cancelled = false;
    gw_cloud_status_t cloud;
    gw_cloud_get_status(&cloud);
    if (cloud.connected) {
        gw_cloud_pause();
        for (int i = 0; i < 30 && !gw_cloud_is_paused(); ++i) vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "voice setup heap internal=%u largest=%u psram=%u mqtt_paused=%d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), gw_cloud_is_paused());
    if (!s_client || !esp_websocket_client_is_connected(s_client) || !s_hello_ok) {
        esp_err_t start_err = gw_voice_ws_start();
        if (start_err != ESP_OK) {
            if (cloud.connected) gw_cloud_resume();
            return start_err;
        }
        for (int i = 0; i < 50 && !s_hello_ok; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_client || !s_hello_ok || !esp_websocket_client_is_connected(s_client)) {
        if (cloud.connected) gw_cloud_resume();
        return ESP_ERR_TIMEOUT;
    }
    /* The server sends MCP initialize/tools/list immediately after hello. Let
     * those replies complete before allocating the Opus encoder and I2S DMA. */
    for (int i = 0; i < 100 && !s_mcp_ready; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    int64_t ready_us = esp_timer_get_time();
    int16_t *handoff = NULL;
    size_t handoff_samples = 0;
    if (continue_mic && !file_pcm) {
        handoff = heap_caps_malloc(16000 * 4 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!handoff) {
            gw_voice_ws_stop();
            if (cloud.connected) gw_cloud_resume();
            return ESP_ERR_NO_MEM;
        }
        handoff_samples = gw_wake_take_handoff(handoff, 16000 * 4);
        ESP_LOGI(TAG, "handoff buffered %.2fs", (double)handoff_samples / 16000.0);
        if (!handoff_samples) {
            free(handoff);
            gw_voice_ws_stop();
            if (cloud.connected) gw_cloud_resume();
            return ESP_ERR_TIMEOUT;
        }
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "session_id", s_session_id);
    cJSON_AddStringToObject(m, "type", "listen");
    cJSON_AddStringToObject(m, "state", "start");
    cJSON_AddStringToObject(m, "mode", auto_mode ? "auto" : "manual");
    char *text = cJSON_PrintUnformatted(m);
    int start_sent = text ? esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(3000)) : -1;
    ESP_LOGI(TAG, "listen start sent=%d", start_sent);
    cJSON_free(text); cJSON_Delete(m);
    if (start_sent <= 0) {
        gw_voice_ws_stop();
        if (cloud.connected) gw_cloud_resume();
        return ESP_FAIL;
    }

    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate=16000; cfg.channel=1; cfg.bits_per_sample=16;
    cfg.bitrate=16000; cfg.frame_duration=ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode=ESP_OPUS_ENC_APPLICATION_VOIP; cfg.complexity=0;
    if (esp_opus_enc_register() != ESP_AUDIO_ERR_OK) {
        gw_voice_ws_stop();
        if (cloud.connected) gw_cloud_resume();
        return ESP_FAIL;
    }
    esp_audio_enc_config_t cc = {.type=ESP_AUDIO_TYPE_OPUS, .cfg=&cfg, .cfg_sz=sizeof(cfg)};
    listen_ctx_t x = {.err=ESP_OK};
    x.capture_limit_ms = 8000;
    if (esp_audio_enc_open(&cc, &x.enc) != ESP_AUDIO_ERR_OK) {
        esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
        gw_voice_ws_stop();
        if (cloud.connected) gw_cloud_resume();
        return ESP_FAIL;
    }
    int pcm_bytes;
    if (esp_audio_enc_get_frame_size(x.enc, &pcm_bytes, &x.packet_capacity) != ESP_AUDIO_ERR_OK) x.err=ESP_FAIL;
    x.frame_samples=pcm_bytes/2;
    x.pcm=heap_caps_malloc(pcm_bytes, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    x.opus=heap_caps_malloc(x.packet_capacity, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!x.pcm || !x.opus) x.err=ESP_ERR_NO_MEM;
    if (x.err == ESP_OK && continue_mic) {
        const int16_t *preroll = file_pcm ? file_pcm : handoff;
        size_t count = file_pcm ? file_samples : handoff_samples;
        if (preroll && count) {
            for (size_t offset = 0; offset < count; offset += 512) {
                size_t n = count - offset > 512 ? 512 : count - offset;
                voice_vad_observe(&x.vad, preroll + offset, n);
            }
            x.err = encode_pcm(preroll, count, &x);
            unsigned buffered_ms = (unsigned)(count * 1000 / 16000);
            x.capture_limit_ms = buffered_ms < 6000 ? 8000 - buffered_ms : 2000;
            if (x.capture_limit_ms < 2000) x.capture_limit_ms = 2000;
        }
    }
    free(handoff);
    if (x.err == ESP_OK) {
        if (file_pcm && file_samples && !continue_mic) {
            for (size_t off = 0; off < file_samples && x.err == ESP_OK; off += 512) {
                size_t n = file_samples - off;
                if (n > 512) n = 512;
                x.err = listen_pcm(file_pcm + off, n, &x);
                vTaskDelay(pdMS_TO_TICKS(n * 1000 / 16000));
            }
        } else {
            x.pcm_queue = xQueueCreateWithCaps(32, sizeof(voice_pcm_frame_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!x.pcm_queue) x.err = ESP_ERR_NO_MEM;
            if (x.err == ESP_OK && xTaskCreatePinnedToCoreWithCaps(
                    voice_capture_task, "voice_capture", 16384, &x, 6, NULL, 0,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) x.err = ESP_ERR_NO_MEM;
            if (x.err == ESP_OK) {
                voice_pcm_frame_t frame;
                while (!x.capture_done || uxQueueMessagesWaiting(x.pcm_queue)) {
                    if (xQueueReceive(x.pcm_queue, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
                        if (encode_pcm(frame.samples, frame.count, &x) != ESP_OK) break;
                    }
                }
            }
            if (x.pcm_queue) {
                while (!x.capture_done) vTaskDelay(pdMS_TO_TICKS(20));
                vQueueDeleteWithCaps(x.pcm_queue);
                x.pcm_queue = NULL;
            }
        }
    }
    int64_t upload_done_us = esp_timer_get_time();
    ESP_LOGI(TAG, "capture complete: %.2fs speech=%d silence=%.2fs dropped=%lu result=%s",
              (double)x.vad.captured_samples / 16000.0, x.vad.speech_seen,
              (double)x.vad.silence_samples / 16000.0, (unsigned long)x.dropped_frames,
             esp_err_to_name(x.err));
    if (!file_pcm || continue_mic) ESP_LOGI(TAG, "latency: setup=%lldms capture=%lldms upload_tail=%lldms sent=%.2fs",
                            (long long)((ready_us - session_started_us) / 1000),
                            (long long)((x.capture_finished_us - x.capture_started_us) / 1000),
                            (long long)((upload_done_us - x.capture_finished_us) / 1000),
                            (double)x.sent_samples / 16000.0);
    free(x.pcm); free(x.opus); esp_audio_enc_close(x.enc); esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
    if (!auto_mode) {
        cJSON *stop = cJSON_CreateObject(); cJSON_AddStringToObject(stop,"session_id",s_session_id);
        cJSON_AddStringToObject(stop,"type","listen"); cJSON_AddStringToObject(stop,"state","stop");
        text=cJSON_PrintUnformatted(stop);
        int stop_sent = text && s_client && esp_websocket_client_is_connected(s_client) ?
            esp_websocket_client_send_text(s_client,text,strlen(text),pdMS_TO_TICKS(3000)) : -1;
        ESP_LOGI(TAG, "listen stop sent=%d opus_frames=%u opus_bytes=%u",
                 stop_sent, (unsigned)x.sent_frames, (unsigned)x.sent_bytes);
        cJSON_free(text); cJSON_Delete(stop);
        if (stop_sent <= 0 && x.err == ESP_OK) x.err = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "auto listen frames=%u bytes=%u", (unsigned)x.sent_frames, (unsigned)x.sent_bytes);
    }
    /* The capture owns a paired RX/TX channel; TTS borrows TX until playback closes. */
    bool barge_started = false;
    if ((continue_mic && !file_pcm) || probe_barge) {
        esp_err_t barge_err = gw_wake_begin_barge_in();
        barge_started = barge_err == ESP_OK;
        ESP_LOGI(TAG, "barge-in microphone: %s", esp_err_to_name(barge_err));
        if (barge_started) {
            for (int i = 0; i < 30 && !gw_wake_barge_in_ready(); ++i) vTaskDelay(pdMS_TO_TICKS(20));
            ESP_LOGI(TAG, "barge-in channels ready=%d", gw_wake_barge_in_ready());
        }
    }
    /* Keep MQTT paused until TTS is finished, with a bounded timeout for
     * silent/failed recognition. */
    for (int i = 0; i < 200 && !s_tts_done && !(barge_started && gw_wake_barge_in_detected()) &&
          s_client && esp_websocket_client_is_connected(s_client); ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (barge_started && gw_wake_barge_in_detected()) {
        ESP_LOGI(TAG, "barge-in: cancelling old TTS/session");
        s_tts_cancelled = true;
    }
    s_tts_end_requested = true;
    if (s_tts_task && xSemaphoreTake(s_tts_finished, pdMS_TO_TICKS(5000)) != pdTRUE)
        ESP_LOGE(TAG, "TTS worker did not finish before websocket close");
    else if (s_tts_queue) { vQueueDeleteWithCaps(s_tts_queue); s_tts_queue = NULL; }
    /* Keep capturing after a real wake word while the next WebSocket session
     * connects. The next session takes the buffered audio before opening its RX. */
    if (barge_started && !gw_wake_barge_in_detected()) gw_wake_end_barge_in();
    gw_voice_ws_stop();
    if (cloud.connected) gw_cloud_resume();
    return x.err;
}

typedef struct { TaskHandle_t waiter; esp_err_t result; const int16_t *pcm; size_t samples; bool auto_mode; bool continue_mic; bool probe_barge; } listen_task_ctx_t;
static void listen_task(void *arg)
{
    listen_task_ctx_t *ctx = arg;
    ctx->result = gw_voice_ws_listen_impl(ctx->pcm, ctx->samples, ctx->auto_mode, ctx->continue_mic, ctx->probe_barge);
    xTaskNotifyGive(ctx->waiter);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t gw_voice_ws_listen(void)
{
    gw_wake_pause();
    listen_task_ctx_t ctx = {.waiter = xTaskGetCurrentTaskHandle(), .result = ESP_FAIL};
    if (xTaskCreateWithCaps(listen_task, "voice_listen", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        gw_wake_resume();
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    gw_wake_resume();
    return ctx.result;
}

esp_err_t gw_voice_ws_listen_with_preroll(const int16_t *pcm, size_t samples)
{
    if (samples > 64000 || (samples && !pcm)) return ESP_ERR_INVALID_ARG;
    listen_task_ctx_t ctx = {.waiter = xTaskGetCurrentTaskHandle(), .result = ESP_FAIL,
                              .pcm = pcm, .samples = samples, .continue_mic = true};
    if (xTaskCreateWithCaps(listen_task, "voice_preroll", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ctx.result;
}

esp_err_t gw_voice_ws_listen_auto(void)
{
    gw_wake_pause();
    listen_task_ctx_t ctx = {.waiter = xTaskGetCurrentTaskHandle(), .result = ESP_FAIL,
                              .auto_mode = true};
    if (xTaskCreateWithCaps(listen_task, "voice_auto", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        gw_wake_resume();
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    gw_wake_resume();
    return ctx.result;
}

esp_err_t gw_voice_ws_listen_pcm(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples || samples > 160000) return ESP_ERR_INVALID_ARG;
    gw_wake_pause();
    listen_task_ctx_t ctx = {.waiter=xTaskGetCurrentTaskHandle(), .result=ESP_FAIL,
                              .pcm=pcm, .samples=samples};
    if (xTaskCreateWithCaps(listen_task, "voice_file", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        gw_wake_resume();
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    gw_wake_resume();
    return ctx.result;
}

esp_err_t gw_voice_ws_probe_barge_pcm(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples || samples > 160000) return ESP_ERR_INVALID_ARG;
    gw_wake_pause();
    listen_task_ctx_t ctx = {.waiter=xTaskGetCurrentTaskHandle(), .result=ESP_FAIL,
                              .pcm=pcm, .samples=samples, .probe_barge=true};
    if (xTaskCreateWithCaps(listen_task, "voice_barge_probe", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        gw_wake_resume();
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ctx.result;
}

esp_err_t gw_voice_ws_listen_pcm_auto(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples || samples > 160000) return ESP_ERR_INVALID_ARG;
    gw_wake_pause();
    listen_task_ctx_t ctx = {.waiter=xTaskGetCurrentTaskHandle(), .result=ESP_FAIL,
                              .pcm=pcm, .samples=samples, .auto_mode=true};
    if (xTaskCreateWithCaps(listen_task, "voice_file_auto", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        gw_wake_resume();
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    gw_wake_resume();
    return ctx.result;
}

static void load_config(void)
{
    nvs_handle_t h;
    s_url[0] = s_token[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_url);
        nvs_get_str(h, "url", s_url, &n);
        n = sizeof(s_token);
        nvs_get_str(h, "token", s_token, &n);
        n = sizeof(s_client_id);
        nvs_get_str(h, "client_id", s_client_id, &n);
        nvs_close(h);
    }
    if (!s_client_id[0] && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t uuid[16];
        esp_fill_random(uuid, sizeof(uuid));
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        snprintf(s_client_id, sizeof(s_client_id),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
                 uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        nvs_set_str(h, "client_id", s_client_id);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t gw_voice_ws_config(const char *url, const char *token)
{
    if (s_client || !url || !token || (url[0] && strncmp(url, "wss://", 6)) ||
        strlen(url) >= sizeof(s_url) || strlen(token) >= sizeof(s_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "url", url);
    if (err == ESP_OK) err = nvs_set_str(h, "token", token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_url, url, sizeof(s_url));
        strlcpy(s_token, token, sizeof(s_token));
    }
    return err;
}

esp_err_t gw_voice_ws_activate(bool confirm)
{
    load_config();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char url[320];
    snprintf(url, sizeof(url), "%s%s", s_activation_url, confirm ? "activate" : "");
    char *body = calloc(1, 16384);
    s_http_body = body; s_http_body_len = 0;
    esp_http_client_config_t cfg = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach,
                                    .timeout_ms = 15000, .buffer_size = 2048,
                                    .event_handler = activation_http_event};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;
    char device[32]; snprintf(device, sizeof(device), MACSTR, MAC2STR(mac));
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Activation-Version", "1");
    esp_http_client_set_header(c, "Device-Id", device);
    esp_http_client_set_header(c, "Client-Id", s_client_id);
    char agent[64];
    snprintf(agent, sizeof(agent), "esp_s3_gateway/%s", esp_app_get_description()->version);
    esp_http_client_set_header(c, "User-Agent", agent);
    esp_http_client_set_header(c, "Accept-Language", "zh-CN");
    esp_http_client_set_post_field(c, "{}", 2);
    gw_cloud_status_t cloud;
    gw_cloud_get_status(&cloud);
    if (!confirm && cloud.connected) gw_cloud_pause();
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    int n = body ? (int)s_http_body_len : -1;
    ESP_LOGI(TAG, "activation HTTP status=%d bytes=%d", status, n);
    if (err == ESP_OK && status == 200 && body) {
        cJSON *root = cJSON_Parse(body);
        cJSON *a = root ? cJSON_GetObjectItem(root, "activation") : NULL;
        if (!confirm && cJSON_IsObject(a)) {
            const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(a, "code"));
            const char *challenge = cJSON_GetStringValue(cJSON_GetObjectItem(a, "challenge"));
            if (code && challenge) {
                strlcpy(s_activation_challenge, challenge, sizeof(s_activation_challenge));
                cJSON *w = cJSON_GetObjectItem(root, "websocket");
                const char *u = cJSON_IsObject(w) ? cJSON_GetStringValue(cJSON_GetObjectItem(w, "url")) : NULL;
                const char *t = cJSON_IsObject(w) ? cJSON_GetStringValue(cJSON_GetObjectItem(w, "token")) : NULL;
                if (u && t) {
                    strlcpy(s_pending_ws_url, u, sizeof(s_pending_ws_url));
                    strlcpy(s_pending_ws_token, t, sizeof(s_pending_ws_token));
                }
                printf("小智设备验证码: %s\n网页绑定完成后执行: voicews activate confirm\n", code);
            } else err = ESP_ERR_INVALID_RESPONSE;
        } else if (confirm) {
            if (status == 200 && s_pending_ws_url[0] && s_pending_ws_token[0]) {
                err = gw_voice_ws_config(s_pending_ws_url, s_pending_ws_token);
                s_pending_ws_url[0] = s_pending_ws_token[0] = 0;
            }
            else if (!s_activation_challenge[0]) err = ESP_ERR_INVALID_STATE;
        }
        if (root) cJSON_Delete(root);
    } else if (err == ESP_OK) err = status == 202 ? ESP_ERR_TIMEOUT : ESP_FAIL;
    free(body);
    s_http_body = NULL;
    esp_http_client_cleanup(c);
    if (!confirm && cloud.connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
        gw_cloud_resume();
    }
    return err;
}

static void send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *features = cJSON_AddObjectToObject(root, "features");
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration", 60);
    char *text = cJSON_PrintUnformatted(root);
    if (text) {
        esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(3000));
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void on_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_websocket_event_data_t *event = data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_hello_ok = false;
        s_session_id[0] = '\0';
        s_mcp_ready = false;
        send_hello();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (event->op_code == WS_TRANSPORT_OPCODES_BINARY && event->data_len > 0 &&
            event->payload_offset == 0 && event->data_len == event->payload_len) {
            if (!s_tts_cancelled && event->data_len <= sizeof(((tts_packet_t *)0)->data) && tts_start_worker()) {
                tts_packet_t packet = {.len = event->data_len};
                memcpy(packet.data, event->data_ptr, packet.len);
                if (xQueueSend(s_tts_queue, &packet, 0) != pdTRUE)
                    ESP_LOGW(TAG, "TTS queue full, dropping packet");
            }
            break;
        }
        if (event->op_code == WS_TRANSPORT_OPCODES_TEXT && event->data_len > 0 &&
            event->payload_offset == 0 && event->data_len == event->payload_len) {
            cJSON *root = cJSON_ParseWithLength(event->data_ptr, event->data_len);
            if (root) {
                const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
                if (type && !strcmp(type, "stt")) {
                    const char *recognized = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
                    ESP_LOGI(TAG, "stt: %s", recognized ? recognized : "");
                } else if (type && !strcmp(type, "tts")) {
                    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
                    ESP_LOGI(TAG, "tts: %s", state ? state : "unknown");
                } else if (type && !strcmp(type, "mcp")) {
                    cJSON *payload = cJSON_GetObjectItem(root, "payload");
                    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "method"));
                    ESP_LOGI(TAG, "mcp: %s", method ? method : "reply");
                }
                if (type && !strcmp(type, "hello")) {
                    const char *transport = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transport"));
                    if (transport && !strcmp(transport, "websocket")) {
                        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_id"));
                        strlcpy(s_session_id, sid ? sid : "", sizeof(s_session_id));
                        s_hello_ok = true;
                        ESP_LOGI(TAG, "server hello accepted");
                    }
                } else if (s_hello_ok && type && !strcmp(type, "mcp")) {
                    cJSON *payload = cJSON_GetObjectItem(root, "payload");
                    cJSON *reply = gw_voice_tools_handle(payload);
                    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "method"));
                    cJSON *envelope = cJSON_CreateObject();
                    cJSON_AddStringToObject(envelope, "type", "mcp");
                    if (s_session_id[0]) {
                        cJSON_AddStringToObject(envelope, "session_id", s_session_id);
                    }
                    cJSON_AddItemToObject(envelope, "payload", reply);
                    char *text = cJSON_PrintUnformatted(envelope);
                    if (text) {
                        int sent = esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(3000));
                        if (sent >= 0 && method && !strcmp(method, "tools/list")) s_mcp_ready = true;
                        cJSON_free(text);
                    }
                    cJSON_Delete(envelope);
                } else if (type && !strcmp(type, "tts")) {
                    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
                    if (state && !strcmp(state, "stop")) {
                        s_tts_end_requested = true;
                        if (!s_tts_task) s_tts_done = true;
                    }
                }
                cJSON_Delete(root);
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_hello_ok = false;
        s_mcp_ready = false;
        s_tts_end_requested = true;
        if (!s_tts_task) s_tts_done = true;
        s_session_id[0] = '\0';
        ESP_LOGW(TAG, "disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "error");
        break;
    default:
        break;
    }
}

esp_err_t gw_voice_ws_init(void)
{
    load_config();
    return ESP_OK;
}

esp_err_t gw_voice_ws_start(void)
{
    if (!s_url[0] || !s_token[0] || !s_client_id[0]) return ESP_ERR_INVALID_STATE;
    if (s_client) {
        if (esp_websocket_client_is_connected(s_client) && s_hello_ok) return ESP_OK;
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_hello_ok = false;
    }
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_headers, sizeof(s_headers),
             "Authorization: Bearer %s\r\nProtocol-Version: 1\r\nDevice-Id: " MACSTR
             "\r\nClient-Id: %s\r\n", s_token, MAC2STR(mac), s_client_id);
    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = s_headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .task_stack = 6144,
        .reconnect_timeout_ms = 10000,
    };
    gw_cloud_status_t cloud;
    gw_cloud_get_status(&cloud);
    if (cloud.connected) gw_cloud_pause();
    vTaskDelay(pdMS_TO_TICKS(300));
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        if (cloud.connected) gw_cloud_resume();
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed: %s internal=%u largest=%u",
                 esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    if (cloud.connected) gw_cloud_resume();
    return err;
}

void gw_voice_ws_stop(void)
{
    if (!s_client) return;
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_hello_ok = false;
}

void gw_voice_ws_status(void)
{
    printf("voice ws: %s hello=%s endpoint=%s\n",
           s_client && esp_websocket_client_is_connected(s_client) ? "connected" : "stopped",
           s_hello_ok ? "ok" : "pending", s_url[0] ? "configured" : "not_configured");
}

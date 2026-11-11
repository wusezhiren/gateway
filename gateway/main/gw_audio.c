#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_reg.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_audio.h"
#include "gw_wake.h"

/* The display initializes the shared board I2C bus on GPIO17/18. */
#define AUDIO_I2C_PORT I2C_NUM_1
#define ES8311_ADDR    0x18
#define ES7210_ADDR    0x40
#define MIC_RATE       16000

/* Keep DMA and capture buffers small: the running gateway has limited internal RAM. */
static int16_t s_mic_buf[512 * 4];
static int s_capture_slot;

esp_err_t gw_audio_probe(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_master_get_bus_handle(AUDIO_I2C_PORT, &bus);
    if (err != ESP_OK) {
        printf("audio I2C bus not initialized: %s\n", esp_err_to_name(err));
        return err;
    }

    const struct {
        const char *name;
        uint8_t addr;
    } codecs[] = {{"ES8311", ES8311_ADDR}, {"ES7210", ES7210_ADDR}};
    bool found_all = true;
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        err = i2c_master_probe(bus, codecs[i].addr, 100);
        printf("%s I2C 0x%02x: %s\n", codecs[i].name, codecs[i].addr,
               err == ESP_OK ? "ACK" : esp_err_to_name(err));
        found_all &= (err == ESP_OK);
    }
    return found_all ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t capture_impl(unsigned duration_ms, gw_audio_pcm_cb_t cb, void *ctx, bool diagnostic)
{
    if (duration_ms == 0 || duration_ms > 30000) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_master_get_bus_handle(AUDIO_I2C_PORT, &bus);
    if (err != ESP_OK) {
        return err;
    }

    i2s_chan_handle_t rx = NULL;
    const audio_codec_ctrl_if_t *ctrl = NULL;
    const audio_codec_data_if_t *data = NULL;
    const audio_codec_if_t *codec_if = NULL;
    esp_codec_dev_handle_t codec = NULL;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Keep internal DMA usage low enough to coexist with MQTT/WebSocket TLS. */
    chan_cfg.dma_desc_num = 2;
    chan_cfg.dma_frame_num = 64;
    err = i2s_new_channel(&chan_cfg, NULL, &rx);
    if (err != ESP_OK) {
        goto done;
    }
    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = GPIO_NUM_16, .bclk = GPIO_NUM_9, .ws = GPIO_NUM_45,
            .dout = I2S_GPIO_UNUSED, .din = GPIO_NUM_10,
        },
    };
    tdm.slot_cfg.total_slot = 4;
    tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_tdm_mode(rx, &tdm);
    if (err != ESP_OK) {
        goto done;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_I2C_PORT, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus,
    };
    ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx};
    data = audio_codec_new_i2s_data(&i2s_cfg);
    if (!ctrl || !data) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = ctrl, .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = 256,
    };
    codec_if = es7210_codec_new(&adc_cfg);
    if (!codec_if) {
        err = ESP_FAIL;
        goto done;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec_if, .data_if = data,
    };
    codec = esp_codec_dev_new(&dev_cfg);
    if (!codec) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16, .channel = 4,
        .channel_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .sample_rate = MIC_RATE,
    };
    if (esp_codec_dev_open(codec, &sample_cfg) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_gain(codec, 20) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto done;
    }
    uint32_t peak[4] = {0};
    uint64_t energy[4] = {0};
    uint32_t count = 0;
    uint32_t reads = 0;
    int64_t capture_started_us = esp_timer_get_time();
    uint32_t target_frames = MIC_RATE * duration_ms / 1000;
    while (count < target_frames) {
        size_t bytes = 0;
        err = i2s_channel_read(rx, s_mic_buf, sizeof(s_mic_buf), &bytes, 1000);
        if (err != ESP_OK) {
            goto done;
        }
        /* esp_codec_dev_open() reconfigures the active TDM mask to MIC1|MIC2
         * (0x3). DMA therefore contains two samples per 16 kHz frame, even
         * though the bus still has four time slots. */
        size_t frames = bytes / (sizeof(int16_t) * 2);
        reads++;
        if (!frames) {
            err = ESP_FAIL;
            goto done;
        }
        if (frames > target_frames - count) {
            frames = target_frames - count;
        }
        for (size_t i = 0; i < frames; i++) {
            for (int ch = 0; ch < 4; ch++) {
                int32_t value = ch < 2 ? s_mic_buf[i * 2 + ch] : 0;
                uint32_t abs_value = value < 0 ? -value : value;
                if (abs_value > peak[ch]) {
                    peak[ch] = abs_value;
                }
                energy[ch] += (uint64_t)((int64_t)value * value);
            }
        }
        if (cb) {
            /* The two microphones may have opposite phase. Averaging them can
             * cancel speech, so use the verified first microphone for mono. */
            for (size_t i = 0; i < frames; i++) {
                s_mic_buf[i] = s_mic_buf[i * 2 + (s_capture_slot == 2 ? 1 : 0)];
            }
            err = cb(s_mic_buf, frames, ctx);
            if (err != ESP_OK) {
                goto done;
            }
        }
        count += frames;
    }
    if (diagnostic) printf("mic frames=%lu peak=%lu,%lu,%lu,%lu mean_square=%llu,%llu,%llu,%llu\n",
           (unsigned long)count, (unsigned long)peak[0], (unsigned long)peak[1],
           (unsigned long)peak[2], (unsigned long)peak[3],
           (unsigned long long)(energy[0] / (count ? count : 1)),
           (unsigned long long)(energy[1] / (count ? count : 1)),
           (unsigned long long)(energy[2] / (count ? count : 1)),
           (unsigned long long)(energy[3] / (count ? count : 1)));
    if (diagnostic) printf("mic timing: frames=%lu reads=%lu elapsed_ms=%lld effective_rate=%lu\n",
                           (unsigned long)count, (unsigned long)reads,
                           (long long)((esp_timer_get_time() - capture_started_us) / 1000),
                           (unsigned long)(count * 1000000LL /
                           (esp_timer_get_time() - capture_started_us + 1)));

done:
    if (codec) {
        esp_codec_dev_close(codec);
        esp_codec_dev_delete(codec);
    }
    if (codec_if) {
        audio_codec_delete_codec_if(codec_if);
    }
    if (data) {
        audio_codec_delete_data_if(data);
    }
    if (ctrl) {
        audio_codec_delete_ctrl_if(ctrl);
    }
    if (rx) {
        i2s_del_channel(rx);
    }
    if (err != ESP_OK && diagnostic) {
        printf("mic test failed: %s\n", esp_err_to_name(err));
    }
    return err;
}

esp_err_t gw_audio_capture(unsigned duration_ms, gw_audio_pcm_cb_t cb, void *ctx)
{
    return capture_impl(duration_ms, cb, ctx, true);
}

esp_err_t gw_audio_capture_quiet(unsigned duration_ms, gw_audio_pcm_cb_t cb, void *ctx)
{
    return capture_impl(duration_ms, cb, ctx, false);
}

typedef struct { int16_t *out; size_t capacity; size_t used; } record_ctx_t;
static esp_err_t record_pcm_cb(const int16_t *samples, size_t count, void *ctx)
{
    record_ctx_t *r = ctx;
    if (count > r->capacity - r->used) return ESP_ERR_INVALID_SIZE;
    memcpy(r->out + r->used, samples, count * sizeof(int16_t));
    r->used += count;
    return ESP_OK;
}

esp_err_t gw_audio_record_pcm(int16_t *out, size_t samples, int slot)
{
    if (!out || samples != 16000 * 4 || (slot != 0 && slot != 2)) return ESP_ERR_INVALID_ARG;
    gw_wake_pause();
    s_capture_slot = slot;
    record_ctx_t ctx = {.out = out, .capacity = samples};
    esp_err_t err = gw_audio_capture_quiet(4000, record_pcm_cb, &ctx);
    s_capture_slot = 0;
    gw_wake_resume();
    return err == ESP_OK && ctx.used == samples ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_audio_mic_test(void)
{
    gw_wake_pause();
    esp_err_t err = gw_audio_capture(2048, NULL, NULL);
    gw_wake_resume();
    return err;
}

esp_err_t gw_audio_speaker_test(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_master_get_bus_handle(AUDIO_I2C_PORT, &bus);
    if (err != ESP_OK) {
        return err;
    }
    i2s_chan_handle_t tx = NULL;
    const audio_codec_ctrl_if_t *ctrl = NULL;
    const audio_codec_data_if_t *data = NULL;
    const audio_codec_gpio_if_t *gpio = NULL;
    const audio_codec_if_t *codec_if = NULL;
    esp_codec_dev_handle_t codec = NULL;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 2;
    chan_cfg.dma_frame_num = 64;
    err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) {
        goto done;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_16, .bclk = GPIO_NUM_9, .ws = GPIO_NUM_45,
            .dout = GPIO_NUM_8, .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        goto done;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus,
    };
    ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .tx_handle = tx};
    data = audio_codec_new_i2s_data(&i2s_cfg);
    gpio = audio_codec_new_gpio();
    if (!ctrl || !data || !gpio) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = ctrl, .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .master_mode = false,
        .use_mclk = true, .pa_pin = GPIO_NUM_48, .pa_reverted = false,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        .mclk_div = 256,
    };
    codec_if = es8311_codec_new(&dac_cfg);
    if (!codec_if) {
        err = ESP_FAIL;
        goto done;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data,
    };
    codec = esp_codec_dev_new(&dev_cfg);
    if (!codec) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03,
        .sample_rate = MIC_RATE,
    };
    if (esp_codec_dev_open(codec, &sample_cfg) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(codec, 55) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(codec, false) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto done;
    }

    int reg31 = 0, reg32 = 0;
    if (esp_codec_dev_read_reg(codec, 0x31, &reg31) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_read_reg(codec, 0x32, &reg32) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto done;
    }
    printf("speaker DAC mute_reg=0x%02x volume_reg=0x%02x\n", reg31, reg32);
    /* 500 Hz, one second; moderate digital level and hardware attenuation. */
    static int16_t samples[256 * 2];
    for (int block = 0; block < 64; block++) {
        for (int frame = 0; frame < 256; frame++) {
            unsigned phase = (unsigned)(block * 256 + frame) % 32;
            int16_t value = (int16_t)(phase < 16 ? (int)phase * 500 - 4000 : 12000 - (int)phase * 500);
            samples[2 * frame] = value;
            samples[2 * frame + 1] = value;
        }
        size_t bytes = 0;
        err = i2s_channel_write(tx, samples, sizeof(samples), &bytes, 1000);
        if (err != ESP_OK || bytes != sizeof(samples)) {
            err = err == ESP_OK ? ESP_FAIL : err;
            goto done;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    printf("speaker tone sent: 500Hz, 1s\n");

done:
    if (codec) {
        esp_codec_dev_close(codec);
        esp_codec_dev_delete(codec);
    }
    if (codec_if) {
        audio_codec_delete_codec_if(codec_if);
    }
    if (gpio) {
        audio_codec_delete_gpio_if(gpio);
    }
    if (data) {
        audio_codec_delete_data_if(data);
    }
    if (ctrl) {
        audio_codec_delete_ctrl_if(ctrl);
    }
    if (tx) {
        i2s_del_channel(tx);
    }
    if (err != ESP_OK) {
        printf("speaker test failed: %s\n", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t play_wav_impl(const uint8_t *wav, size_t len)
{
    if (!wav || len < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return ESP_ERR_INVALID_ARG;
    uint16_t channels = wav[22] | ((uint16_t)wav[23] << 8);
    uint32_t rate = wav[24] | ((uint32_t)wav[25] << 8) | ((uint32_t)wav[26] << 16) | ((uint32_t)wav[27] << 24);
    uint16_t bits = wav[34] | ((uint16_t)wav[35] << 8);
    if (channels != 1 || bits != 16 || rate != 24000) return ESP_ERR_INVALID_ARG;
    size_t pos = 12, data = 0, data_len = 0;
    while (pos + 8 <= len) { uint32_t n = wav[pos+4] | ((uint32_t)wav[pos+5]<<8) | ((uint32_t)wav[pos+6]<<16) | ((uint32_t)wav[pos+7]<<24); if (!memcmp(wav+pos,"data",4)) { data=pos+8; data_len=n; break; } pos += 8+n; }
    if (!data || data + data_len > len) return ESP_ERR_INVALID_ARG;
    i2c_master_bus_handle_t bus; if (i2c_master_get_bus_handle(AUDIO_I2C_PORT,&bus)!=ESP_OK) return ESP_FAIL;
    i2s_chan_handle_t tx=NULL; const audio_codec_ctrl_if_t *ctrl=NULL; const audio_codec_data_if_t *di=NULL; const audio_codec_gpio_if_t *gpio=NULL; const audio_codec_if_t *ci=NULL; esp_codec_dev_handle_t codec=NULL; esp_err_t err=ESP_OK;
    i2s_chan_config_t cc=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER); cc.dma_desc_num=2; cc.dma_frame_num=64; if(i2s_new_channel(&cc,&tx,NULL)!=ESP_OK) return ESP_ERR_NO_MEM;
    i2s_std_config_t sc={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(24000),.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),.gpio_cfg={.mclk=GPIO_NUM_16,.bclk=GPIO_NUM_9,.ws=GPIO_NUM_45,.dout=GPIO_NUM_8,.din=I2S_GPIO_UNUSED}}; sc.clk_cfg.mclk_multiple=I2S_MCLK_MULTIPLE_256;
    if(i2s_channel_init_std_mode(tx,&sc)!=ESP_OK) {err=ESP_FAIL;goto out;} audio_codec_i2c_cfg_t ic={.port=AUDIO_I2C_PORT,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=bus}; ctrl=audio_codec_new_i2c_ctrl(&ic); audio_codec_i2s_cfg_t id={.port=I2S_NUM_0,.tx_handle=tx}; di=audio_codec_new_i2s_data(&id); gpio=audio_codec_new_gpio(); es8311_codec_cfg_t ec={.ctrl_if=ctrl,.gpio_if=gpio,.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.master_mode=false,.use_mclk=true,.pa_pin=GPIO_NUM_48,.mclk_div=256,.hw_gain={.pa_voltage=5.0f,.codec_dac_voltage=3.3f}}; ci=es8311_codec_new(&ec); esp_codec_dev_cfg_t dc={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=ci,.data_if=di}; codec=esp_codec_dev_new(&dc); esp_codec_dev_sample_info_t si={.bits_per_sample=16,.channel=2,.channel_mask=3,.sample_rate=24000}; if(!codec||esp_codec_dev_open(codec,&si)!=ESP_CODEC_DEV_OK||esp_codec_dev_set_out_vol(codec,55)!=ESP_CODEC_DEV_OK){err=ESP_FAIL;goto out;}
    int16_t *stereo=heap_caps_malloc(1024*sizeof(int16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!stereo){err=ESP_ERR_NO_MEM;goto out;}
    for(size_t off=0;off<data_len;){size_t samples=(data_len-off)/2; if(samples>512)samples=512; const int16_t *in=(const int16_t *)(wav+data+off); for(size_t i=0;i<samples;i++){stereo[2*i]=in[i];stereo[2*i+1]=in[i];} if(esp_codec_dev_write(codec,stereo,samples*4)!=ESP_CODEC_DEV_OK){err=ESP_FAIL;break;} off+=samples*2;}
    free(stereo);
out: if(codec){esp_codec_dev_close(codec);esp_codec_dev_delete(codec);} if(ci)audio_codec_delete_codec_if(ci);if(gpio)audio_codec_delete_gpio_if(gpio);if(di)audio_codec_delete_data_if(di);if(ctrl)audio_codec_delete_ctrl_if(ctrl);if(tx)i2s_del_channel(tx);return err;
}

esp_err_t gw_audio_play_wav_sync(const uint8_t *wav, size_t len)
{
    return play_wav_impl(wav, len);
}

typedef struct { uint8_t *wav; size_t len; } wav_task_ctx_t;
static void wav_task(void *arg) { wav_task_ctx_t *x=arg; gw_wake_pause(); esp_err_t err=play_wav_impl(x->wav,x->len); printf("wav playback: %s\n",esp_err_to_name(err)); gw_wake_resume(); free(x->wav); free(x); vTaskDeleteWithCaps(NULL); }
esp_err_t gw_audio_play_wav(const uint8_t *wav, size_t len)
{
    uint8_t *copy=malloc(len); if(!copy) return ESP_ERR_NO_MEM; memcpy(copy,wav,len);
    wav_task_ctx_t *x=malloc(sizeof(*x)); if(!x){free(copy);return ESP_ERR_NO_MEM;} x->wav=copy;x->len=len;
    if(xTaskCreateWithCaps(wav_task,"wav_play",40960,x,5,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){free(copy);free(x);return ESP_ERR_NO_MEM;} return ESP_OK;
}

typedef struct {
    esp_audio_enc_handle_t encoder;
    int16_t *pcm;
    uint8_t *packet;
    size_t frame_samples;
    size_t used;
    int packet_capacity;
    unsigned frames;
    unsigned bytes;
    int64_t encode_us;
} opus_test_t;

static esp_err_t opus_test_pcm(const int16_t *samples, size_t count, void *ctx)
{
    opus_test_t *test = ctx;
    while (count) {
        size_t n = test->frame_samples - test->used;
        if (n > count) {
            n = count;
        }
        memcpy(test->pcm + test->used, samples, n * sizeof(int16_t));
        test->used += n;
        samples += n;
        count -= n;
        if (test->used != test->frame_samples) {
            continue;
        }
        esp_audio_enc_in_frame_t input = {
            .buffer = (uint8_t *)test->pcm,
            .len = test->frame_samples * sizeof(int16_t),
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = test->packet,
            .len = test->packet_capacity,
        };
        int64_t start = esp_timer_get_time();
        esp_audio_err_t enc_err = esp_audio_enc_process(test->encoder, &input, &output);
        test->encode_us += esp_timer_get_time() - start;
        if (enc_err != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0 ||
            output.encoded_bytes > test->packet_capacity) {
            printf("opus encode failed: %d, bytes=%lu\n", enc_err,
                   (unsigned long)output.encoded_bytes);
            return ESP_FAIL;
        }
        test->frames++;
        test->bytes += output.encoded_bytes;
        test->used = 0;
    }
    return ESP_OK;
}

static esp_err_t gw_audio_opus_test_impl(void)
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = MIC_RATE;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = 16000;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 0;
    opus_test_t test = {0};
    if (esp_opus_enc_register() != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    esp_audio_enc_config_t common_cfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg = &cfg,
        .cfg_sz = sizeof(cfg),
    };
    esp_audio_err_t enc_err = esp_audio_enc_open(&common_cfg, &test.encoder);
    if (enc_err != ESP_AUDIO_ERR_OK || !test.encoder) {
        printf("opus open failed: %d\n", enc_err);
        esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
        return ESP_FAIL;
    }
    int pcm_bytes = 0;
    if (esp_audio_enc_get_frame_size(test.encoder, &pcm_bytes, &test.packet_capacity) != ESP_AUDIO_ERR_OK ||
        pcm_bytes != MIC_RATE * 60 / 1000 * (int)sizeof(int16_t) || test.packet_capacity <= 0) {
        printf("opus frame format mismatch: pcm=%d packet=%d\n", pcm_bytes, test.packet_capacity);
        esp_audio_enc_close(test.encoder);
        esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
        return ESP_FAIL;
    }
    test.frame_samples = pcm_bytes / sizeof(int16_t);
    /* The prebuilt Opus routine performs SIMD/word loads; keep its active frame
     * buffers in internal 8-bit RAM even though larger audio queues can use PSRAM. */
    test.pcm = heap_caps_malloc(pcm_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    test.packet = heap_caps_malloc(test.packet_capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!test.pcm || !test.packet) {
        printf("opus buffers allocation failed\n");
        free(test.pcm);
        free(test.packet);
        esp_audio_enc_close(test.encoder);
        esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
        return ESP_ERR_NO_MEM;
    }
    printf("opus input=%d output_capacity=%d heap_internal=%u psram=%u\n",
           pcm_bytes, test.packet_capacity,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    esp_err_t err = gw_audio_capture(2040, opus_test_pcm, &test);
    printf("opus frames=%u bytes=%u encode_us=%lld heap_internal=%u psram=%u\n",
           test.frames, test.bytes, (long long)test.encode_us,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    free(test.pcm);
    free(test.packet);
    esp_audio_enc_close(test.encoder);
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
    return err;
}

typedef struct {
    TaskHandle_t waiter;
    esp_err_t result;
} opus_task_ctx_t;

static void opus_test_task(void *arg)
{
    opus_task_ctx_t *ctx = arg;
    ctx->result = gw_audio_opus_test_impl();
    xTaskNotifyGive(ctx->waiter);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t gw_audio_opus_test(void)
{
    opus_task_ctx_t ctx = {.waiter = xTaskGetCurrentTaskHandle(), .result = ESP_FAIL};
    if (xTaskCreateWithCaps(opus_test_task, "opus_test", 40960, &ctx, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        printf("opus test task allocation failed\n");
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ctx.result;
}

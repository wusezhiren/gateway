#include "s31_audio.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"

static const char *TAG = "s31_audio";
static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_microphone;

esp_err_t s31_audio_init(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    s_microphone = bsp_audio_codec_microphone_init();
    if (!s_speaker || !s_microphone) return ESP_ERR_NOT_FOUND;

    esp_codec_dev_sample_info_t speaker_fs = {
        .bits_per_sample = 16, .channel = 2, .sample_rate = 48000,
    };
    esp_codec_dev_sample_info_t microphone_fs = {
        .bits_per_sample = 16, .channel = 2, .sample_rate = 16000,
    };
    if (esp_codec_dev_open(s_speaker, &speaker_fs) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(s_microphone, &microphone_fs) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_speaker, 55);
    esp_codec_dev_set_in_gain(s_microphone, 24.0f);
    bsp_audio_poweramp_enable(true);
    ESP_LOGI(TAG, "ES8389 ready: capture 16k stereo, playback 48k stereo");
    return ESP_OK;
}

esp_err_t s31_audio_capture(int16_t *samples, size_t sample_count, size_t *read_samples)
{
    if (!s_microphone || !samples || !read_samples) return ESP_ERR_INVALID_ARG;
    esp_err_t err = esp_codec_dev_read(s_microphone, samples,
                                       sample_count * sizeof(int16_t));
    *read_samples = err == ESP_OK ? sample_count : 0;
    return err;
}

esp_err_t s31_audio_play(const int16_t *samples, size_t sample_count)
{
    if (!s_speaker || !samples || !sample_count) return ESP_ERR_INVALID_ARG;
    return esp_codec_dev_write(s_speaker, samples,
                               sample_count * sizeof(int16_t));
}

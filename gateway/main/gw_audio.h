#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* Synchronous callback: 16 kHz signed 16-bit mono PCM, valid until return.
 * Return an error to cancel capture. Call from a worker, never an ESP-NOW callback.
 * Audio diagnostics and capture must be serialized by the caller.
 */
typedef esp_err_t (*gw_audio_pcm_cb_t)(const int16_t *samples, size_t count, void *ctx);
esp_err_t gw_audio_capture(unsigned duration_ms, gw_audio_pcm_cb_t cb, void *ctx);
esp_err_t gw_audio_capture_quiet(unsigned duration_ms, gw_audio_pcm_cb_t cb, void *ctx);

/* Probe the two audio codecs on the I2C bus already owned by the display. */
esp_err_t gw_audio_probe(void);

/* Short, on-demand dual-mic capture diagnostic; prints levels only, never raw audio. */
esp_err_t gw_audio_mic_test(void);

/* Low-volume, short speaker diagnostic. */
esp_err_t gw_audio_speaker_test(void);
esp_err_t gw_audio_play_wav(const uint8_t *wav, size_t len);
esp_err_t gw_audio_play_wav_sync(const uint8_t *wav, size_t len);
esp_err_t gw_audio_record_pcm(int16_t *out, size_t samples, int slot);

/* Capture 2 seconds and encode 16 kHz mono PCM into 60 ms Opus frames. */
esp_err_t gw_audio_opus_test(void);

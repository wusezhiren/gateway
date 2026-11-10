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

/* Probe the two audio codecs on the I2C bus already owned by the display. */
esp_err_t gw_audio_probe(void);

/* Short, on-demand dual-mic capture diagnostic; prints levels only, never raw audio. */
esp_err_t gw_audio_mic_test(void);

/* Low-volume, short speaker diagnostic. */
esp_err_t gw_audio_speaker_test(void);

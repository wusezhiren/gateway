#pragma once
#include <stddef.h>
#include <stdint.h>

/* Streaming 24 kHz mono PCM to 16 kHz interleaved stereo PCM. */
typedef struct {
    int16_t pending[3];
    size_t count;
} tts_resample_t;

/* Returns output frames; capacity is measured in stereo frames. */
size_t tts_resample_24_to_16(tts_resample_t *state, const int16_t *input,
                             size_t samples, int16_t *output, size_t capacity);

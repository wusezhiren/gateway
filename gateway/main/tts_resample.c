#include "tts_resample.h"

size_t tts_resample_24_to_16(tts_resample_t *state, const int16_t *input,
                             size_t samples, int16_t *output, size_t capacity)
{
    if (!state || (!input && samples) || !output || state->count > 2) return 0;
    size_t produced = 0;
    for (size_t i = 0; i < samples; ++i) {
        if (state->count == 2 && capacity - produced < 2) return produced;
        state->pending[state->count++] = input[i];
        if (state->count != 3) continue;
        int16_t a = state->pending[0];
        int16_t b = (int16_t)(((int32_t)state->pending[1] + state->pending[2]) / 2);
        output[2 * produced] = a;
        output[2 * produced + 1] = a;
        produced++;
        output[2 * produced] = b;
        output[2 * produced + 1] = b;
        produced++;
        state->count = 0;
    }
    return produced;
}

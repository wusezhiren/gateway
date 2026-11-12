#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t captured_samples;
    size_t silence_samples;
    size_t speech_candidate_samples;
    bool speech_seen;
} voice_vad_t;

void voice_vad_observe(voice_vad_t *state, const int16_t *samples, size_t count);
bool voice_vad_should_stop(const voice_vad_t *state, bool queued_capture);
bool voice_vad_should_stop_live(const voice_vad_t *state, size_t live_samples);

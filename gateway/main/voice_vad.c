#include "voice_vad.h"

void voice_vad_observe(voice_vad_t *state, const int16_t *samples, size_t count)
{
    if (!state || (!samples && count) || !count) return;
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        energy += (int64_t)sample * sample;
    }
    state->captured_samples += count;
    /* The Korvo mic can be recognized by STT below 60 RMS. */
    if (energy / count > 24U * 24U) {
        state->speech_candidate_samples += count;
        if (state->speech_candidate_samples >= 2048) state->speech_seen = true;
        state->silence_samples = 0;
    } else {
        state->speech_candidate_samples = 0;
        if (state->speech_seen) state->silence_samples += count;
    }
}

bool voice_vad_should_stop(const voice_vad_t *state, bool queued_capture)
{
    if (!state || !state->speech_seen) return false;
    return queued_capture
        ? state->captured_samples > 32000 && state->silence_samples >= 12800
        : state->captured_samples > 24000 && state->silence_samples >= 16000;
}

bool voice_vad_should_stop_live(const voice_vad_t *state, size_t live_samples)
{
    return live_samples >= 16000 && voice_vad_should_stop(state, true);
}

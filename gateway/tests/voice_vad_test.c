#include <assert.h>
#include <stdio.h>
#include "voice_vad.h"

static void feed(voice_vad_t *vad, int16_t amplitude, size_t samples)
{
    int16_t frame[512];
    for (size_t i = 0; i < 512; ++i) frame[i] = amplitude;
    while (samples) {
        size_t n = samples < 512 ? samples : 512;
        voice_vad_observe(vad, frame, n);
        samples -= n;
    }
}

int main(void)
{
    voice_vad_t vad = {0};
    assert(!voice_vad_should_stop(&vad, true));
    voice_vad_observe(NULL, NULL, 512);
    voice_vad_observe(&vad, NULL, 512);
    assert(vad.captured_samples == 0);
    feed(&vad, 25, 2048);
    assert(vad.speech_seen && !voice_vad_should_stop(&vad, true));
    feed(&vad, 0, 32000);
    assert(voice_vad_should_stop(&vad, true));
    assert(voice_vad_should_stop(&vad, false));
    assert(!voice_vad_should_stop_live(&vad, 15999));
    assert(voice_vad_should_stop_live(&vad, 16000));

    vad = (voice_vad_t){0};
    feed(&vad, 24, 80000); /* below threshold, no false endpoint */
    assert(!vad.speech_seen && !voice_vad_should_stop(&vad, true));
    feed(&vad, 25, 1536);
    feed(&vad, 0, 512);
    feed(&vad, 25, 512);
    assert(!vad.speech_seen); /* short bursts must not accumulate */
    feed(&vad, 25, 2048);
    feed(&vad, 0, 12800);
    assert(voice_vad_should_stop(&vad, true));
    assert(!voice_vad_should_stop(&vad, false));
    feed(&vad, 25, 512);
    assert(!voice_vad_should_stop(&vad, true)); /* speech resets silence */
    assert(!voice_vad_should_stop_live(&vad, 32000));
    puts("voice VAD tests passed");
    return 0;
}

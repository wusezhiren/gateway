#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "tts_resample.h"

int main(void)
{
    tts_resample_t state = {0};
    int16_t out[16] = {0};
    int16_t first[] = {100, 200};
    assert(tts_resample_24_to_16(&state, first, 2, out, 8) == 0);
    assert(state.count == 2);
    int16_t second[] = {300, -300, -200, -100};
    assert(tts_resample_24_to_16(&state, second, 4, out, 8) == 4);
    int16_t expected[] = {100, 100, 250, 250, -300, -300, -150, -150};
    for (size_t i = 0; i < 8; ++i) assert(out[i] == expected[i]);
    assert(state.count == 0);
    assert(tts_resample_24_to_16(NULL, first, 2, out, 8) == 0);
    assert(tts_resample_24_to_16(&state, NULL, 1, out, 8) == 0);
    assert(tts_resample_24_to_16(&state, first, 2, NULL, 8) == 0);
    assert(tts_resample_24_to_16(&state, second, 1, out, 0) == 0);
    assert(state.count == 1);
    state.count = 0;

    int16_t frame[1440] = {0}; /* 60 ms at 24 kHz */
    int16_t playback[1920];
    assert(tts_resample_24_to_16(&state, frame, 1440, playback, 960) == 960);
    assert(state.count == 0);
    for (size_t i = 0; i < 1920; ++i) assert(playback[i] == 0);

    /* An Opus decoder may split a frame at an arbitrary sample boundary. */
    assert(tts_resample_24_to_16(&state, frame, 721, playback, 960) == 480);
    assert(state.count == 1);
    assert(tts_resample_24_to_16(&state, frame + 721, 719, playback, 960) == 480);
    assert(state.count == 0);
    puts("tts resampler passed");
    return 0;
}

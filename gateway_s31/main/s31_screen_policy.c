#include "s31_screen_policy.h"

s31_screen_action_t s31_screen_policy_step(s31_screen_policy_t *state,
                                            uint32_t idle_ms, uint32_t timeout_ms,
                                            bool touching)
{
    if (!state || !timeout_ms) return S31_SCREEN_NONE;
    if (state->waiting_release) {
        if (!touching) state->waiting_release = false;
        return S31_SCREEN_NONE;
    }
    if (state->asleep) {
        if (touching) {
            state->asleep = false;
            state->waiting_release = true;
            return S31_SCREEN_WAKE;
        }
    } else if (idle_ms >= timeout_ms && !touching) {
        state->asleep = true;
        return S31_SCREEN_SLEEP;
    }
    return S31_SCREEN_NONE;
}

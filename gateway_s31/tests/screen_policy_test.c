#include <assert.h>
#include <stdio.h>
#include "s31_screen_policy.h"

int main(void)
{
    s31_screen_policy_t state = {0};
    assert(s31_screen_policy_step(&state, 14999, 15000, false) == S31_SCREEN_NONE);
    assert(s31_screen_policy_step(&state, 15000, 15000, false) == S31_SCREEN_SLEEP);
    assert(state.asleep);
    /* Voice/network events have no input to the screen policy and cannot wake it. */
    assert(s31_screen_policy_step(&state, 60000, 15000, false) == S31_SCREEN_NONE);
    assert(s31_screen_policy_step(&state, 60000, 15000, true) == S31_SCREEN_WAKE);
    assert(!state.asleep && state.waiting_release);
    assert(s31_screen_policy_step(&state, 60000, 15000, true) == S31_SCREEN_NONE);
    assert(s31_screen_policy_step(&state, 0, 15000, false) == S31_SCREEN_NONE);
    assert(!state.waiting_release);
    assert(s31_screen_policy_step(&state, 15000, 15000, true) == S31_SCREEN_NONE);
    assert(s31_screen_policy_step(NULL, 15000, 15000, false) == S31_SCREEN_NONE);
    assert(s31_screen_policy_step(&state, 15000, 0, false) == S31_SCREEN_NONE);
    puts("S31 screen policy positive/negative tests passed");
}

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool asleep;
    bool waiting_release;
} s31_screen_policy_t;

typedef enum { S31_SCREEN_NONE, S31_SCREEN_SLEEP, S31_SCREEN_WAKE } s31_screen_action_t;

s31_screen_action_t s31_screen_policy_step(s31_screen_policy_t *state,
                                            uint32_t idle_ms, uint32_t timeout_ms,
                                            bool touching);

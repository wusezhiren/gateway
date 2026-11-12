#pragma once
#include <stdbool.h>

typedef enum {
    VOICE_IDLE,
    VOICE_LISTENING,
    VOICE_REPLYING,
    VOICE_CANCELLING,
} voice_phase_t;

typedef struct {
    voice_phase_t phase;
    bool pending_wake;
} voice_session_t;

/* Returns true only if the wake word is accepted for a new/queued command. */
bool voice_session_wake(voice_session_t *session);
bool voice_session_reply(voice_session_t *session);
/* Returns true when teardown has completed and a new command can start. */
bool voice_session_closed(voice_session_t *session);

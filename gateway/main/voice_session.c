#include "voice_session.h"

bool voice_session_wake(voice_session_t *session)
{
    if (!session) return false;
    if (session->phase == VOICE_IDLE) {
        session->phase = VOICE_LISTENING;
        return true;
    }
    if (session->phase == VOICE_REPLYING && !session->pending_wake) {
        session->pending_wake = true;
        session->phase = VOICE_CANCELLING;
        return true;
    }
    return false;
}

bool voice_session_reply(voice_session_t *session)
{
    if (!session || session->phase != VOICE_LISTENING) return false;
    session->phase = VOICE_REPLYING;
    return true;
}

bool voice_session_closed(voice_session_t *session)
{
    if (!session || session->phase == VOICE_IDLE) return false;
    bool next = session->phase == VOICE_CANCELLING && session->pending_wake;
    session->pending_wake = false;
    session->phase = next ? VOICE_LISTENING : VOICE_IDLE;
    return next;
}

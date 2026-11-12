#include <assert.h>
#include <stdio.h>
#include "voice_session.h"

int main(void)
{
    voice_session_t s = {0};
    assert(!voice_session_wake(NULL));
    assert(!voice_session_reply(NULL));
    assert(!voice_session_closed(NULL));
    assert(!voice_session_reply(&s));
    assert(voice_session_wake(&s) && s.phase == VOICE_LISTENING);
    assert(!voice_session_wake(&s)); /* command recording cannot be interrupted */
    assert(voice_session_reply(&s) && s.phase == VOICE_REPLYING);
    assert(!voice_session_reply(&s));
    assert(voice_session_wake(&s) && s.phase == VOICE_CANCELLING);
    assert(s.pending_wake && !voice_session_wake(&s)); /* coalesce duplicate wake */
    assert(!voice_session_reply(&s));
    assert(voice_session_closed(&s) && s.phase == VOICE_LISTENING);
    assert(!s.pending_wake && !voice_session_wake(&s));
    assert(voice_session_reply(&s));
    assert(!voice_session_closed(&s) && s.phase == VOICE_IDLE);
    assert(!voice_session_closed(&s));
    assert(voice_session_wake(&s));
    assert(!voice_session_closed(&s) && s.phase == VOICE_IDLE); /* failed session */
    puts("voice session tests passed");
    return 0;
}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_core/voice_core.h"

static unsigned opened, closed;
static voice_audio_direction_t expected_direction = VOICE_AUDIO_CAPTURE;
static voice_status_t open_audio(void *ctx, voice_audio_direction_t direction)
{
    (void)ctx;
    assert(direction == expected_direction);
    expected_direction = direction == VOICE_AUDIO_CAPTURE ? VOICE_AUDIO_PLAYBACK : VOICE_AUDIO_CAPTURE;
    ++opened;
    return VOICE_OK;
}
static void close_audio(void *ctx) { (void)ctx; ++closed; }

static void test_simplex(void)
{
    voice_core_t v;
    voice_ports_t ports = {.audio_open = open_audio, .audio_close = close_audio,
                           .supports_duplex = false};
    assert(voice_core_init(&v, VOICE_AUDIO_SIMPLEX, &ports) == VOICE_OK);
    assert(voice_core_state(&v) == VOICE_STATE_IDLE);
    assert(voice_core_on_wake(&v) == VOICE_OK);
    unsigned generation = voice_core_generation(&v);
    assert(generation != 0 && voice_core_state(&v) == VOICE_STATE_LISTENING);
    assert(voice_core_on_speech_end(&v) == VOICE_OK);
    assert(closed == 1 && voice_core_state(&v) == VOICE_STATE_WAITING_REPLY);
    assert(voice_core_on_tts_start(&v, generation) == VOICE_OK);
    assert(opened == 2 && voice_core_state(&v) == VOICE_STATE_PLAYING);
    assert(voice_core_on_wake(&v) == VOICE_ERR_STATE); /* no acoustic barge-in */
    assert(voice_core_on_tts_end(&v, generation) == VOICE_OK);
    assert(closed == 2 && voice_core_state(&v) == VOICE_STATE_IDLE);
    assert(voice_core_on_wake(&v) == VOICE_OK);
    assert(voice_core_generation(&v) != generation);
    assert(voice_core_on_tts_start(&v, generation) == VOICE_ERR_STALE);
    assert(voice_core_cancel(&v) == VOICE_OK);
    assert(voice_core_cancel(&v) == VOICE_OK);
    assert(opened == closed);
}

static voice_status_t fail_audio(void *ctx, voice_audio_direction_t direction)
{
    (void)ctx;
    (void)direction;
    return VOICE_ERR_UNSUPPORTED;
}

static void test_capability_and_failure(void)
{
    voice_core_t v;
    voice_ports_t ports = {.audio_open = open_audio, .audio_close = close_audio};
    assert(voice_core_init(&v, VOICE_AUDIO_FULL_DUPLEX, &ports) == VOICE_ERR_UNSUPPORTED);
    assert(voice_core_init(&v, VOICE_AUDIO_SIMPLEX, &ports) == VOICE_OK);
    assert(voice_core_on_tts_start(&v, 0) == VOICE_ERR_STATE);
    assert(voice_core_on_speech_end(&v) == VOICE_ERR_STATE);
    assert(voice_core_cancel(&v) == VOICE_OK);
    ports.audio_open = fail_audio;
    assert(voice_core_init(&v, VOICE_AUDIO_SIMPLEX, &ports) == VOICE_OK);
    assert(voice_core_on_wake(&v) == VOICE_ERR_UNSUPPORTED);
    assert(voice_core_state(&v) == VOICE_STATE_IDLE);
}

static void test_duplex_barge(void)
{
    expected_direction = VOICE_AUDIO_CAPTURE;
    voice_core_t v;
    voice_ports_t ports = {.audio_open = fail_audio, .audio_close = close_audio,
                           .supports_duplex = true};
    assert(voice_core_init(&v, VOICE_AUDIO_FULL_DUPLEX, &ports) == VOICE_OK);
    assert(voice_core_on_barge_in(&v) == VOICE_ERR_STATE);
    /* Use a hardware-free fake to exercise cancellation and stale TTS callbacks. */
    ports.audio_open = open_audio;
    assert(voice_core_init(&v, VOICE_AUDIO_FULL_DUPLEX, &ports) == VOICE_OK);
    assert(voice_core_on_wake(&v) == VOICE_OK);
    unsigned old = voice_core_generation(&v);
    assert(voice_core_on_speech_end(&v) == VOICE_OK);
    assert(voice_core_on_tts_start(&v, old) == VOICE_OK);
    assert(voice_core_on_barge_in(&v) == VOICE_OK);
    assert(voice_core_has_pending_wake(&v));
    assert(voice_core_on_tts_end(&v, old) == VOICE_ERR_STALE);
    assert(voice_core_on_session_closed(&v) == VOICE_OK);
    assert(voice_core_state(&v) == VOICE_STATE_LISTENING);
    assert(voice_core_generation(&v) != old);
    assert(voice_core_cancel(&v) == VOICE_OK);
    assert(!voice_core_has_pending_wake(&v));
}

static voice_status_t set_node(void *ctx, const char *args, char *out, size_t cap)
{
    int *called = ctx;
    ++*called;
    assert(args && *args);
    return snprintf(out, cap, "{\"status\":\"accepted_pending_node_report\"}") < (int)cap ? VOICE_OK : VOICE_ERR_SIZE;
}

static void test_mcp(void)
{
    voice_mcp_t mcp = {0};
    int called = 0;
    assert(voice_mcp_register(&mcp, "gateway.node.set", set_node, &called) == VOICE_OK);
    assert(voice_mcp_register(&mcp, "gateway.node.set", set_node, &called) == VOICE_ERR_DUPLICATE);
    char out[96];
    assert(voice_mcp_call(&mcp, "unknown", "{}", out, sizeof(out)) == VOICE_ERR_NOT_FOUND);
    assert(voice_mcp_call(&mcp, "gateway.node.set", "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"ch\":0,\"value\":1}", out, sizeof(out)) == VOICE_OK);
    assert(called == 1 && strstr(out, "accepted_pending_node_report"));
    assert(voice_mcp_call(&mcp, "gateway.node.set", "{}", out, 4) == VOICE_ERR_SIZE);
}

int main(void)
{
    test_simplex();
    test_capability_and_failure();
    test_duplex_barge();
    test_mcp();
    puts("voice_core contract tests passed");
}

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    VOICE_OK = 0,
    VOICE_ERR_ARGUMENT,
    VOICE_ERR_STATE,
    VOICE_ERR_UNSUPPORTED,
    VOICE_ERR_STALE,
    VOICE_ERR_DUPLICATE,
    VOICE_ERR_NOT_FOUND,
    VOICE_ERR_SIZE,
} voice_status_t;

typedef enum { VOICE_AUDIO_SIMPLEX, VOICE_AUDIO_FULL_DUPLEX } voice_audio_mode_t;
typedef enum { VOICE_AUDIO_CAPTURE, VOICE_AUDIO_PLAYBACK } voice_audio_direction_t;
typedef enum {
    VOICE_STATE_IDLE,
    VOICE_STATE_LISTENING,
    VOICE_STATE_WAITING_REPLY,
    VOICE_STATE_PLAYING,
} voice_state_t;

typedef struct {
    voice_status_t (*audio_open)(void *ctx, voice_audio_direction_t direction);
    void (*audio_close)(void *ctx);
    void *ctx;
    bool supports_duplex;
} voice_ports_t;

typedef struct {
    voice_ports_t ports;
    voice_audio_mode_t mode;
    voice_state_t state;
    unsigned generation;
    bool audio_open;
    bool pending_wake;
} voice_core_t;

voice_status_t voice_core_init(voice_core_t *core, voice_audio_mode_t mode, const voice_ports_t *ports);
voice_status_t voice_core_on_wake(voice_core_t *core);
voice_status_t voice_core_on_speech_end(voice_core_t *core);
voice_status_t voice_core_on_tts_start(voice_core_t *core, unsigned generation);
voice_status_t voice_core_on_tts_end(voice_core_t *core, unsigned generation);
voice_status_t voice_core_cancel(voice_core_t *core);
/* Full-duplex wake during playback: caller cancels old transport/TTS, then closes it. */
voice_status_t voice_core_on_barge_in(voice_core_t *core);
voice_status_t voice_core_on_session_closed(voice_core_t *core);
bool voice_core_has_pending_wake(const voice_core_t *core);
voice_state_t voice_core_state(const voice_core_t *core);
unsigned voice_core_generation(const voice_core_t *core);

/* Raw dispatch only: a later MCP/JSON-RPC layer validates schema before calling.
 * Callbacks run on the application's worker task, never on the WS event callback. */
typedef voice_status_t (*voice_mcp_execute_t)(void *ctx, const char *arguments_json,
                                             char *result_json, size_t capacity);
typedef bool (*voice_mcp_validate_t)(void *ctx, const char *arguments_json);
#define VOICE_MCP_MAX_TOOLS 16
typedef struct {
    const char *name;
    voice_mcp_execute_t execute;
    voice_mcp_validate_t validate;
    const char *description;
    const char *schema_json;
    void *ctx;
} voice_mcp_tool_t;
typedef struct {
    voice_mcp_tool_t tools[VOICE_MCP_MAX_TOOLS];
    size_t count;
} voice_mcp_t;

voice_status_t voice_mcp_register(voice_mcp_t *mcp, const char *name,
                                  voice_mcp_execute_t execute, void *ctx);
voice_status_t voice_mcp_register_tool(voice_mcp_t *mcp, const char *name,
    const char *description, const char *schema_json, voice_mcp_validate_t validate,
    voice_mcp_execute_t execute, void *ctx);
voice_status_t voice_mcp_call(const voice_mcp_t *mcp, const char *name,
                              const char *arguments_json, char *result_json, size_t capacity);

#include "voice_core/voice_core.h"

#include <string.h>

static void audio_close(voice_core_t *core)
{
    if (core->audio_open) {
        core->ports.audio_close(core->ports.ctx);
        core->audio_open = false;
    }
}

voice_status_t voice_core_init(voice_core_t *core, voice_audio_mode_t mode, const voice_ports_t *ports)
{
    if (!core || !ports || !ports->audio_open || !ports->audio_close ||
        (mode != VOICE_AUDIO_SIMPLEX && mode != VOICE_AUDIO_FULL_DUPLEX)) return VOICE_ERR_ARGUMENT;
    if (mode == VOICE_AUDIO_FULL_DUPLEX && !ports->supports_duplex) return VOICE_ERR_UNSUPPORTED;
    *core = (voice_core_t){.ports = *ports, .mode = mode, .state = VOICE_STATE_IDLE};
    return VOICE_OK;
}

voice_status_t voice_core_on_wake(voice_core_t *core)
{
    if (!core) return VOICE_ERR_ARGUMENT;
    if (core->state != VOICE_STATE_IDLE) return VOICE_ERR_STATE;
    voice_status_t result = core->ports.audio_open(core->ports.ctx, VOICE_AUDIO_CAPTURE);
    if (result != VOICE_OK) return result;
    core->audio_open = true;
    ++core->generation;
    if (core->generation == 0) ++core->generation;
    core->state = VOICE_STATE_LISTENING;
    return VOICE_OK;
}

voice_status_t voice_core_on_speech_end(voice_core_t *core)
{
    if (!core || core->state != VOICE_STATE_LISTENING) return VOICE_ERR_STATE;
    audio_close(core);
    core->state = VOICE_STATE_WAITING_REPLY;
    return VOICE_OK;
}

voice_status_t voice_core_on_tts_start(voice_core_t *core, unsigned generation)
{
    if (!core) return VOICE_ERR_ARGUMENT;
    if (generation != core->generation) return VOICE_ERR_STALE;
    if (core->state != VOICE_STATE_WAITING_REPLY) return VOICE_ERR_STATE;
    voice_status_t result = core->ports.audio_open(core->ports.ctx, VOICE_AUDIO_PLAYBACK);
    if (result != VOICE_OK) return result;
    core->audio_open = true;
    core->state = VOICE_STATE_PLAYING;
    return VOICE_OK;
}

voice_status_t voice_core_on_tts_end(voice_core_t *core, unsigned generation)
{
    if (!core) return VOICE_ERR_ARGUMENT;
    if (generation != core->generation) return VOICE_ERR_STALE;
    if (core->state != VOICE_STATE_PLAYING) return VOICE_ERR_STATE;
    audio_close(core);
    core->state = VOICE_STATE_IDLE;
    return VOICE_OK;
}

voice_status_t voice_core_cancel(voice_core_t *core)
{
    if (!core) return VOICE_ERR_ARGUMENT;
    audio_close(core);
    core->state = VOICE_STATE_IDLE;
    core->pending_wake = false;
    return VOICE_OK;
}

voice_status_t voice_core_on_barge_in(voice_core_t *core)
{
    if (!core) return VOICE_ERR_ARGUMENT;
    if (core->mode != VOICE_AUDIO_FULL_DUPLEX) return VOICE_ERR_UNSUPPORTED;
    if (core->state != VOICE_STATE_PLAYING) return VOICE_ERR_STATE;
    core->pending_wake = true;
    ++core->generation;
    if (core->generation == 0) ++core->generation;
    return VOICE_OK;
}

voice_status_t voice_core_on_session_closed(voice_core_t *core)
{
    if (!core) return VOICE_ERR_ARGUMENT;
    if (!core->pending_wake) return VOICE_ERR_STATE;
    audio_close(core);
    core->state = VOICE_STATE_IDLE;
    core->pending_wake = false;
    voice_status_t result = core->ports.audio_open(core->ports.ctx, VOICE_AUDIO_CAPTURE);
    if (result != VOICE_OK) return result;
    core->audio_open = true;
    core->state = VOICE_STATE_LISTENING;
    return VOICE_OK;
}

bool voice_core_has_pending_wake(const voice_core_t *core)
{
    return core && core->pending_wake;
}

voice_state_t voice_core_state(const voice_core_t *core)
{
    return core ? core->state : VOICE_STATE_IDLE;
}

unsigned voice_core_generation(const voice_core_t *core)
{
    return core ? core->generation : 0;
}

voice_status_t voice_mcp_register(voice_mcp_t *mcp, const char *name,
                                  voice_mcp_execute_t execute, void *ctx)
{
    return voice_mcp_register_tool(mcp, name, "", "{\"type\":\"object\"}", NULL, execute, ctx);
}

voice_status_t voice_mcp_register_tool(voice_mcp_t *mcp, const char *name,
    const char *description, const char *schema_json, voice_mcp_validate_t validate,
    voice_mcp_execute_t execute, void *ctx)
{
    if (!mcp || !name || !*name || !description || !schema_json || !execute) return VOICE_ERR_ARGUMENT;
    for (size_t i = 0; i < mcp->count; ++i)
        if (strcmp(mcp->tools[i].name, name) == 0) return VOICE_ERR_DUPLICATE;
    if (mcp->count >= VOICE_MCP_MAX_TOOLS) return VOICE_ERR_SIZE;
    mcp->tools[mcp->count++] = (voice_mcp_tool_t){.name = name, .execute = execute,
        .validate = validate, .description = description, .schema_json = schema_json, .ctx = ctx};
    return VOICE_OK;
}

voice_status_t voice_mcp_call(const voice_mcp_t *mcp, const char *name,
                              const char *arguments_json, char *result_json, size_t capacity)
{
    if (!mcp || !name || !arguments_json || !result_json || !capacity) return VOICE_ERR_ARGUMENT;
    result_json[0] = '\0';
    for (size_t i = 0; i < mcp->count; ++i) {
        if (strcmp(mcp->tools[i].name, name) == 0) {
            if (mcp->tools[i].validate && !mcp->tools[i].validate(mcp->tools[i].ctx, arguments_json))
                return VOICE_ERR_ARGUMENT;
            return mcp->tools[i].execute(mcp->tools[i].ctx, arguments_json, result_json, capacity);
        }
    }
    return VOICE_ERR_NOT_FOUND;
}

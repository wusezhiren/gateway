#pragma once

#include "voice_core.h"

/* Parse one JSON-RPC MCP payload and write one JSON-RPC response.
 * Invoke from a worker task; tool callbacks must not run on the transport callback. */
voice_status_t voice_mcp_handle_json(const voice_mcp_t *mcp, const char *request,
                                     char *response, size_t capacity);
voice_status_t voice_mcp_handle_json_ex(const voice_mcp_t *mcp, const char *request,
    const char *server_name, const char *server_version, char *response, size_t capacity);

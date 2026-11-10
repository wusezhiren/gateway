#pragma once

#include "cJSON.h"

/* Caller owns authentication and must cJSON_Delete the returned JSON-RPC reply.
 * Only node listing and channel setting are exposed to the voice backend.
 */
cJSON *gw_voice_tools_handle(const cJSON *request);

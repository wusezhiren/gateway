#include "voice_core/voice_mcp_json.h"

#include <string.h>
#include "cJSON.h"

static cJSON *error_reply(const cJSON *id, int code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return root;
}

voice_status_t voice_mcp_handle_json(const voice_mcp_t *mcp, const char *request,
                                     char *response, size_t capacity)
{
    return voice_mcp_handle_json_ex(mcp, request, "voice-core", "0", response, capacity);
}

voice_status_t voice_mcp_handle_json_ex(const voice_mcp_t *mcp, const char *request,
    const char *server_name, const char *server_version, char *response, size_t capacity)
{
    if (!mcp || !request || !server_name || !server_version || !response || !capacity) return VOICE_ERR_ARGUMENT;
    response[0] = '\0';
    cJSON *input = cJSON_ParseWithOpts(request, NULL, 1);
    cJSON *reply = NULL;
    const cJSON *id = input ? cJSON_GetObjectItemCaseSensitive(input, "id") : NULL;
    if (!input) {
        reply = error_reply(NULL, -32700, "parse error");
    } else if (!cJSON_IsObject(input) || !id || !(cJSON_IsString(id) || cJSON_IsNumber(id)) ||
               !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(input, "jsonrpc")) ||
               strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "jsonrpc")), "2.0")) {
        reply = error_reply(NULL, -32600, "invalid request");
    } else {
        const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "method"));
        if (method && (!strcmp(method, "initialize") || !strcmp(method, "tools/list"))) {
            reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
            cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, true));
            cJSON *result = cJSON_AddObjectToObject(reply, "result");
            if (!strcmp(method, "initialize")) {
                cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
                cJSON_AddObjectToObject(result, "capabilities");
                cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
                cJSON_AddStringToObject(info, "name", server_name);
                cJSON_AddStringToObject(info, "version", server_version);
            } else {
                cJSON *tools = cJSON_AddArrayToObject(result, "tools");
                for (size_t i = 0; i < mcp->count; ++i) {
                    cJSON *schema = cJSON_ParseWithOpts(mcp->tools[i].schema_json, NULL, 1);
                    if (!cJSON_IsObject(schema)) { cJSON_Delete(schema); cJSON_Delete(reply); reply = NULL; break; }
                    cJSON *tool = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool, "name", mcp->tools[i].name);
                    cJSON_AddStringToObject(tool, "description", mcp->tools[i].description);
                    cJSON_AddItemToObject(tool, "inputSchema", schema);
                    cJSON_AddItemToArray(tools, tool);
                }
            }
        } else if (!method || strcmp(method, "tools/call")) {
            reply = error_reply(id, -32601, "method not found");
        } else {
            const cJSON *params = cJSON_GetObjectItemCaseSensitive(input, "params");
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "name"));
            const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
            if (!cJSON_IsObject(params) || !name || !cJSON_IsObject(args)) {
                reply = error_reply(id, -32602, "invalid params");
            } else {
                char *args_text = cJSON_PrintUnformatted(args);
                char result_text[1024] = {0};
                voice_status_t status = args_text ? voice_mcp_call(mcp, name, args_text,
                                                result_text, sizeof(result_text)) : VOICE_ERR_SIZE;
                cJSON_free(args_text);
                if (status == VOICE_ERR_NOT_FOUND) {
                    reply = error_reply(id, -32601, "unknown tool");
                } else if (status == VOICE_ERR_ARGUMENT) {
                    reply = error_reply(id, -32602, "invalid tool arguments");
                } else if (status != VOICE_OK) {
                    reply = error_reply(id, -32603, "tool failed");
                } else {
                    cJSON *result = cJSON_ParseWithOpts(result_text, NULL, 1);
                    if (!cJSON_IsObject(result)) {
                        cJSON_Delete(result);
                        reply = error_reply(id, -32603, "invalid tool result");
                    } else {
                        reply = cJSON_CreateObject();
                        cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
                        cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, true));
                        cJSON_AddItemToObject(reply, "result", result);
                    }
                }
            }
        }
    }
    cJSON_Delete(input);
    if (!reply) return VOICE_ERR_SIZE;
    char *text = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (!text) return VOICE_ERR_SIZE;
    size_t length = strlen(text);
    if (length >= capacity) { cJSON_free(text); return VOICE_ERR_SIZE; }
    memcpy(response, text, length + 1);
    cJSON_free(text);
    return VOICE_OK;
}

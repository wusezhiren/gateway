#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "gw_api.h"
#include "gw_voice_tools.h"
#include "voice_core/voice_mcp_json.h"

static voice_mcp_t s_tools;
static bool s_registered;
static bool valid_mac(const char *mac);
static bool integer_in_range(const cJSON *value, int max);

static bool valid_set_args(void *ctx, const char *json)
{
    (void)ctx;
    cJSON *args = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsObject(args)) { cJSON_Delete(args); return false; }
    const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(args, "mac"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
    const cJSON *ch = cJSON_GetObjectItem(args, "ch");
    const cJSON *value = cJSON_GetObjectItem(args, "value");
    bool ok = ((mac && valid_mac(mac)) || (name && *name)) &&
              (!mac || valid_mac(mac)) && (!name || *name) &&
              integer_in_range(ch, 255) && integer_in_range(value, 65535) &&
              cJSON_GetArraySize(args) == 3 + (mac && name ? 1 : 0);
    cJSON_Delete(args);
    return ok;
}

static cJSON *tool_result(const cJSON *payload, bool error)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    char *json = cJSON_PrintUnformatted(payload);
    cJSON_AddStringToObject(text, "text", json ? json : "{}");
    cJSON_free(json);
    cJSON_AddItemToArray(content, text);
    cJSON_AddBoolToObject(result, "isError", error);
    return result;
}

static bool integer_in_range(const cJSON *value, int max)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= 0 && value->valuedouble <= max &&
           value->valuedouble == (double)value->valueint;
}

static bool valid_mac(const char *mac)
{
    if (!mac || strlen(mac) != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        char c = mac[i];
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static voice_status_t write_result(cJSON *result, char *out, size_t capacity)
{
    if (!result) return VOICE_ERR_SIZE;
    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (!text) return VOICE_ERR_SIZE;
    size_t len = strlen(text);
    if (len >= capacity) { cJSON_free(text); return VOICE_ERR_SIZE; }
    memcpy(out, text, len + 1);
    cJSON_free(text);
    return VOICE_OK;
}

static voice_status_t list_nodes(void *ctx, const char *json, char *out, size_t capacity)
{
    (void)ctx; (void)json;
    cJSON *nodes = gw_api_nodes_json();
    cJSON *result = tool_result(nodes, false);
    cJSON_Delete(nodes);
    return write_result(result, out, capacity);
}

static voice_status_t set_node(void *ctx, const char *json, char *out, size_t capacity)
{
    (void)ctx;
    cJSON *args = cJSON_ParseWithOpts(json, NULL, true);
    if (!args) return VOICE_ERR_ARGUMENT;
    const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(args, "mac"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
    char resolved_mac[18] = {0};
    const char *resolution_error = NULL;
    if (name) {
        cJSON *snapshot = gw_api_nodes_json();
        const cJSON *nodes = cJSON_GetObjectItem(snapshot, "nodes");
        int matches = 0;
        const cJSON *node;
        cJSON_ArrayForEach(node, nodes) {
            const char *current = cJSON_GetStringValue(cJSON_GetObjectItem(node, "name"));
            if (!current || strcmp(current, name)) continue;
            const char *candidate = cJSON_GetStringValue(cJSON_GetObjectItem(node, "mac"));
            if (candidate && (!mac || strcasecmp(candidate, mac) == 0)) {
                strlcpy(resolved_mac, candidate, sizeof(resolved_mac));
                matches++;
            }
        }
        cJSON_Delete(snapshot);
        if (matches != 1) resolution_error = matches ? "ambiguous_name" : "name_not_found";
    }
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "op", "set");
    cJSON_AddStringToObject(command, "mac", name ? resolved_mac : mac);
    cJSON_AddNumberToObject(command, "ch", cJSON_GetObjectItem(args, "ch")->valueint);
    cJSON_AddNumberToObject(command, "value", cJSON_GetObjectItem(args, "value")->valueint);
    const char *error = resolution_error ? resolution_error : gw_api_exec(command);
    cJSON_Delete(command);
    cJSON_Delete(args);
    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, error ? "error" : "status",
                            error ? error : "accepted_pending_node_report");
    cJSON *result = tool_result(status, error != NULL);
    cJSON_Delete(status);
    return write_result(result, out, capacity);
}

static bool no_args(void *ctx, const char *json)
{
    (void)ctx;
    cJSON *args = cJSON_ParseWithOpts(json, NULL, true);
    bool valid = cJSON_IsObject(args) && !args->child;
    cJSON_Delete(args);
    return valid;
}

static void register_tools(void)
{
    if (s_registered) return;
    static const char list_schema[] =
        "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{}}";
    static const char set_schema[] =
        "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
        "\"mac\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},"
        "\"ch\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"}},"
        "\"required\":[\"ch\",\"value\"],\"anyOf\":[{\"required\":[\"mac\"]},{\"required\":[\"name\"]}]}";
    voice_mcp_register_tool(&s_tools, "gateway.nodes.list",
        "List paired ESP-NOW nodes and actual channel values", list_schema,
        no_args, list_nodes, NULL);
    voice_mcp_register_tool(&s_tools, "gateway.node.set",
        "Set a paired node channel by its current name or MAC. Call gateway.nodes.list for current names; duplicate names require MAC.", set_schema,
        valid_set_args, set_node, NULL);
    s_registered = true;
}

cJSON *gw_voice_tools_handle(const cJSON *request)
{
    register_tools();
    char *json = cJSON_PrintUnformatted(request);
    if (!json) return NULL;
    char *response = malloc(8192);
    if (!response) { cJSON_free(json); return NULL; }
    voice_status_t err = voice_mcp_handle_json_ex(&s_tools, json, "esp-s3-gateway",
        esp_app_get_description()->version, response, 8192);
    cJSON_free(json);
    cJSON *result = err == VOICE_OK ? cJSON_Parse(response) : NULL;
    free(response);
    return result;
}

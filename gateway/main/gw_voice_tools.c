#include <math.h>
#include <string.h>
#include "gw_api.h"
#include "gw_voice_tools.h"

static cJSON *reply(const cJSON *id, cJSON *result, int code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id && (cJSON_IsString(id) || cJSON_IsNumber(id))) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(root, "id");
    }
    if (message) {
        cJSON_Delete(result);
        cJSON *error = cJSON_AddObjectToObject(root, "error");
        cJSON_AddNumberToObject(error, "code", code);
        cJSON_AddStringToObject(error, "message", message);
    } else {
        cJSON_AddItemToObject(root, "result", result ? result : cJSON_CreateObject());
    }
    return root;
}

static cJSON *tool_schema(const char *name, const char *description, bool setting)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", description);
    cJSON *schema = cJSON_AddObjectToObject(tool, "inputSchema");
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddBoolToObject(schema, "additionalProperties", false);
    cJSON *properties = cJSON_AddObjectToObject(schema, "properties");
    if (setting) {
        cJSON *mac = cJSON_AddObjectToObject(properties, "mac");
        cJSON_AddStringToObject(mac, "type", "string");
        cJSON *ch = cJSON_AddObjectToObject(properties, "ch");
        cJSON_AddStringToObject(ch, "type", "integer");
        cJSON *value = cJSON_AddObjectToObject(properties, "value");
        cJSON_AddStringToObject(value, "type", "integer");
        cJSON *required = cJSON_AddArrayToObject(schema, "required");
        cJSON_AddItemToArray(required, cJSON_CreateString("mac"));
        cJSON_AddItemToArray(required, cJSON_CreateString("ch"));
        cJSON_AddItemToArray(required, cJSON_CreateString("value"));
    }
    return tool;
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

cJSON *gw_voice_tools_handle(const cJSON *request)
{
    if (!cJSON_IsObject(request)) {
        return reply(NULL, NULL, -32600, "invalid request");
    }
    const cJSON *id = cJSON_GetObjectItem(request, "id");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(request, "jsonrpc"));
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(request, "method"));
    if (!version || strcmp(version, "2.0") || !method ||
        !id || !(cJSON_IsNumber(id) || cJSON_IsString(id))) {
        return reply(id, NULL, -32600, "invalid request");
    }
    if (!strcmp(method, "tools/list")) {
        cJSON *result = cJSON_CreateObject();
        cJSON *tools = cJSON_AddArrayToObject(result, "tools");
        cJSON_AddItemToArray(tools, tool_schema("gateway.nodes.list",
            "List paired ESP-NOW nodes and actual channel values", false));
        cJSON_AddItemToArray(tools, tool_schema("gateway.node.set",
            "Send a channel setting to an already paired ESP-NOW node", true));
        return reply(id, result, 0, NULL);
    }
    if (strcmp(method, "tools/call")) {
        return reply(id, NULL, -32601, "method not found");
    }
    const cJSON *params = cJSON_GetObjectItem(request, "params");
    if (!cJSON_IsObject(params)) {
        return reply(id, NULL, -32602, "params required");
    }
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
    const cJSON *args = cJSON_GetObjectItem(params, "arguments");
    if (!name || !cJSON_IsObject(args)) {
        return reply(id, NULL, -32602, "tool name and arguments required");
    }
    if (!strcmp(name, "gateway.nodes.list")) {
        if (args->child) {
            return reply(id, NULL, -32602, "list takes no arguments");
        }
        cJSON *nodes = gw_api_nodes_json();
        cJSON *result = tool_result(nodes, false);
        cJSON_Delete(nodes);
        return reply(id, result, 0, NULL);
    }
    if (strcmp(name, "gateway.node.set")) {
        return reply(id, NULL, -32601, "unknown tool");
    }
    const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(args, "mac"));
    const cJSON *ch = cJSON_GetObjectItem(args, "ch");
    const cJSON *value = cJSON_GetObjectItem(args, "value");
    if (!valid_mac(mac) || !integer_in_range(ch, 255) || !integer_in_range(value, 65535) ||
        cJSON_GetArraySize(args) != 3) {
        return reply(id, NULL, -32602, "invalid mac, ch or value");
    }
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "op", "set");
    cJSON_AddStringToObject(command, "mac", mac);
    cJSON_AddNumberToObject(command, "ch", ch->valueint);
    cJSON_AddNumberToObject(command, "value", value->valueint);
    const char *err = gw_api_exec(command);
    cJSON_Delete(command);
    cJSON *status = cJSON_CreateObject();
    if (err) {
        cJSON_AddStringToObject(status, "error", err);
    } else {
        cJSON_AddStringToObject(status, "status", "accepted_pending_node_report");
    }
    cJSON *result = tool_result(status, err != NULL);
    cJSON_Delete(status);
    return reply(id, result, 0, NULL);
}

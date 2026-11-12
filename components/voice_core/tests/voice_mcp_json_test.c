#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_core/voice_mcp_json.h"

static unsigned calls;
static voice_status_t execute(void *ctx, const char *args, char *out, size_t cap)
{
    (void)ctx;
    assert(strstr(args, "\"ch\":0"));
    ++calls;
    return snprintf(out, cap, "{\"status\":\"accepted_pending_node_report\"}") < (int)cap ? VOICE_OK : VOICE_ERR_SIZE;
}

int main(void)
{
    voice_mcp_t mcp = {0};
    char out[512];
    assert(voice_mcp_register(&mcp, "gateway.node.set", execute, NULL) == VOICE_OK);
    assert(voice_mcp_handle_json_ex(&mcp, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}",
        "esp-s3-gateway", "0.7.0", out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "esp-s3-gateway") && strstr(out, "2024-11-05"));
    assert(voice_mcp_handle_json(&mcp, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
        out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "gateway.node.set") && strstr(out, "inputSchema"));
    assert(voice_mcp_handle_json(&mcp,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"gateway.node.set\",\"arguments\":{\"ch\":0}}}",
        out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "\"id\":7") && strstr(out, "accepted_pending_node_report") && calls == 1);
    assert(voice_mcp_handle_json(&mcp,
        "{\"jsonrpc\":\"2.0\",\"id\":\"req-1\",\"method\":\"tools/call\",\"params\":{\"name\":\"missing\",\"arguments\":{}}}",
        out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "\"id\":\"req-1\"") && strstr(out, "\"code\":-32601") && calls == 1);
    assert(voice_mcp_handle_json(&mcp,
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"gateway.node.set\",\"arguments\":[]}}",
        out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "\"code\":-32602") && calls == 1);
    assert(voice_mcp_handle_json(&mcp, "{broken", out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "\"code\":-32700") && strstr(out, "\"id\":null"));
    assert(voice_mcp_handle_json(&mcp, "{} trailing", out, sizeof(out)) == VOICE_OK);
    assert(strstr(out, "\"code\":-32700"));
    assert(voice_mcp_handle_json(&mcp, "{}", out, 8) == VOICE_ERR_SIZE);
    puts("voice MCP JSON tests passed");
}

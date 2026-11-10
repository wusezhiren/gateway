#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "gw_voice_tools.h"
#include "gw_voice_ws.h"

static const char *TAG = "gw_voice_ws";
#define NS "gw_voice"
static char s_url[256];
static char s_token[192];
static char s_headers[512];
static char s_client_id[37];
static char s_session_id[96];
static bool s_hello_ok;
static esp_websocket_client_handle_t s_client;

static void load_config(void)
{
    nvs_handle_t h;
    s_url[0] = s_token[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_url);
        nvs_get_str(h, "url", s_url, &n);
        n = sizeof(s_token);
        nvs_get_str(h, "token", s_token, &n);
        n = sizeof(s_client_id);
        nvs_get_str(h, "client_id", s_client_id, &n);
        nvs_close(h);
    }
    if (!s_client_id[0] && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t uuid[16];
        esp_fill_random(uuid, sizeof(uuid));
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        snprintf(s_client_id, sizeof(s_client_id),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
                 uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        nvs_set_str(h, "client_id", s_client_id);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t gw_voice_ws_config(const char *url, const char *token)
{
    if (s_client || !url || !token || (url[0] && strncmp(url, "wss://", 6)) ||
        strlen(url) >= sizeof(s_url) || strlen(token) >= sizeof(s_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "url", url);
    if (err == ESP_OK) err = nvs_set_str(h, "token", token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_url, url, sizeof(s_url));
        strlcpy(s_token, token, sizeof(s_token));
    }
    return err;
}

static void send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *features = cJSON_AddObjectToObject(root, "features");
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration", 60);
    char *text = cJSON_PrintUnformatted(root);
    if (text) {
        esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(3000));
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void on_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_websocket_event_data_t *event = data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_hello_ok = false;
        s_session_id[0] = '\0';
        send_hello();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (event->op_code == WS_TRANSPORT_OPCODES_TEXT && event->data_len > 0 &&
            event->payload_offset == 0 && event->data_len == event->payload_len) {
            cJSON *root = cJSON_ParseWithLength(event->data_ptr, event->data_len);
            if (root) {
                const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
                if (type && !strcmp(type, "hello")) {
                    const char *transport = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transport"));
                    if (transport && !strcmp(transport, "websocket")) {
                        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_id"));
                        strlcpy(s_session_id, sid ? sid : "", sizeof(s_session_id));
                        s_hello_ok = true;
                        ESP_LOGI(TAG, "server hello accepted");
                    }
                } else if (s_hello_ok && type && !strcmp(type, "mcp")) {
                    cJSON *payload = cJSON_GetObjectItem(root, "payload");
                    cJSON *reply = gw_voice_tools_handle(payload);
                    cJSON *envelope = cJSON_CreateObject();
                    cJSON_AddStringToObject(envelope, "type", "mcp");
                    if (s_session_id[0]) {
                        cJSON_AddStringToObject(envelope, "session_id", s_session_id);
                    }
                    cJSON_AddItemToObject(envelope, "payload", reply);
                    char *text = cJSON_PrintUnformatted(envelope);
                    if (text) {
                        esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(3000));
                        cJSON_free(text);
                    }
                    cJSON_Delete(envelope);
                }
                cJSON_Delete(root);
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_hello_ok = false;
        s_session_id[0] = '\0';
        ESP_LOGW(TAG, "disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "error");
        break;
    default:
        break;
    }
}

esp_err_t gw_voice_ws_init(void)
{
    load_config();
    return ESP_OK;
}

esp_err_t gw_voice_ws_start(void)
{
    if (!s_url[0] || !s_token[0] || !s_client_id[0]) return ESP_ERR_INVALID_STATE;
    if (s_client) return ESP_OK;
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_headers, sizeof(s_headers),
             "Authorization: Bearer %s\r\nProtocol-Version: 1\r\nDevice-Id: " MACSTR
             "\r\nClient-Id: %s\r\n", s_token, MAC2STR(mac), s_client_id);
    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = s_headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .task_stack = 8192,
        .reconnect_timeout_ms = 10000,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_ERR_NO_MEM;
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    return err;
}

void gw_voice_ws_stop(void)
{
    if (!s_client) return;
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_hello_ok = false;
}

void gw_voice_ws_status(void)
{
    printf("voice ws: %s hello=%s endpoint=%s\n",
           s_client && esp_websocket_client_is_connected(s_client) ? "connected" : "stopped",
           s_hello_ok ? "ok" : "pending", s_url[0] ? "configured" : "not_configured");
}

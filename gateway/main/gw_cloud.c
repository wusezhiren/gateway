#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "gw_api.h"
#include "gw_cloud.h"
#include "gw_wifi.h"

static const char *TAG = "gw_cloud";

#define NVS_NS              "gw_cloud"
#define TOPIC_PREFIX        "espgw"
#define STATE_MIN_GAP_US    (300 * 1000)        /* 两次状态上报的最小间隔 */
#define STATE_PERIOD_US     (30 * 1000000LL)    /* 无变化时也定期上报（运行时间、信号等） */

static char s_id[13];
static char s_t_state[40], s_t_online[40], s_t_cmd[40], s_t_resp[40];
static char s_pass[65];

static gw_cloud_status_t s_st;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_task;
static volatile bool s_reconfig;
static volatile bool s_dirty = true;

const char *gw_cloud_id(void)
{
    return s_id;
}

void gw_cloud_get_status(gw_cloud_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
}

void gw_cloud_get_password(char *out, size_t len)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_pass, len);
    taskEXIT_CRITICAL(&s_lock);
}

static void set_connected(bool c, const char *err)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.connected = c;
    if (err) {
        strlcpy(s_st.error, err, sizeof(s_st.error));
    } else if (c) {
        s_st.error[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_lock);
}

void gw_cloud_notify(void)
{
    s_dirty = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ---------- 配置 ---------- */

static void load_config(void)
{
    nvs_handle_t h;
    gw_cloud_status_t st = {0};
    char pass[sizeof(s_pass)] = "";
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(st.host);
        if (nvs_get_str(h, "host", st.host, &len) != ESP_OK) {
            st.host[0] = '\0';
        }
        len = sizeof(st.user);
        if (nvs_get_str(h, "user", st.user, &len) != ESP_OK) {
            st.user[0] = '\0';
        }
        len = sizeof(pass);
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
        nvs_get_u16(h, "port", &st.port);
        nvs_close(h);
    }
    st.configured = st.host[0] != '\0';
    taskENTER_CRITICAL(&s_lock);
    s_st = st;
    strlcpy(s_pass, pass, sizeof(s_pass));
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t gw_cloud_config(const char *host, int port, const char *user, const char *pass)
{
    if (strlen(host) >= sizeof(s_st.host) || strlen(user) >= sizeof(s_st.user) ||
        strlen(pass) >= sizeof(s_pass) || port <= 0 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (host[0]) {
        nvs_set_str(h, "host", host);
        nvs_set_u16(h, "port", (uint16_t)port);
        nvs_set_str(h, "user", user);
        nvs_set_str(h, "pass", pass);
    } else {
        nvs_erase_all(h);
    }
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config %s %s:%d user=%s", host[0] ? "set" : "cleared", host, port, user);
    s_reconfig = true;
    gw_cloud_notify();
    return err;
}

/* ---------- 发布 ---------- */

static void publish_state(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "info", gw_api_info_json());
    cJSON_AddItemToObject(o, "nodes", gw_api_nodes_json());
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        esp_mqtt_client_publish(s_client, s_t_state, s, 0, 1, 1);
        free(s);
    }
}

static void reply(const char *id, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddBoolToObject(o, "ok", err == NULL);
    if (err) {
        cJSON_AddStringToObject(o, "error", err);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        esp_mqtt_client_publish(s_client, s_t_resp, s, 0, 1, 0);
        free(s);
    }
}

/* 命令：{"id": "...", "token": "<API token>", "op": "set", ...}，在 MQTT 任务里执行（都很快） */
static void handle_cmd(const char *data, int len)
{
    cJSON *cmd = cJSON_ParseWithLength(data, len);
    if (!cJSON_IsObject(cmd)) {
        cJSON_Delete(cmd);
        return;
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "id"));
    const char *err;
    if (!gw_api_token_ok(cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "token")))) {
        err = "unauthorized";
    } else {
        err = gw_api_exec(cmd);
    }
    ESP_LOGI(TAG, "cmd %s: %s", cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "op")) ?: "?", err ?: "ok");
    reply(id, err);
    cJSON_Delete(cmd);
    gw_cloud_notify();
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        set_connected(true, NULL);
        esp_mqtt_client_publish(s_client, s_t_online, "1", 1, 1, 1);
        esp_mqtt_client_subscribe(s_client, s_t_cmd, 1);
        gw_cloud_notify();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        set_connected(false, NULL);
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "refused: %d", ev->error_handle->connect_return_code);
            set_connected(false, "auth_failed");
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: tls 0x%x sock %d", ev->error_handle->esp_tls_last_esp_err,
                     ev->error_handle->esp_transport_sock_errno);
            set_connected(false, ev->error_handle->esp_tls_last_esp_err ? "tls_failed" : "connect_failed");
        }
        break;
    case MQTT_EVENT_DATA:
        /* 命令都很短，不处理分片 */
        if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len &&
            ev->topic_len == (int)strlen(s_t_cmd) && memcmp(ev->topic, s_t_cmd, ev->topic_len) == 0) {
            handle_cmd(ev->data, ev->data_len);
        }
        break;
    default:
        break;
    }
}

/* ---------- 连接管理 ---------- */

static void client_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    set_connected(false, NULL);
}

static void client_start(void)
{
    gw_cloud_status_t st;
    gw_cloud_get_status(&st);
    char client_id[24];
    snprintf(client_id, sizeof(client_id), "espgw-%s", s_id);
    esp_mqtt_client_config_t cfg = {
        .broker.address = {
            .hostname = st.host,
            .port = st.port ? st.port : 8883,
            .transport = MQTT_TRANSPORT_OVER_SSL,
        },
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials = {
            .client_id = client_id,
            .username = st.user,
            .authentication.password = s_pass,
        },
        .session = {
            .keepalive = 60,
            .last_will = {.topic = s_t_online, .msg = "0", .msg_len = 1, .qos = 1, .retain = 1},
        },
        .network = {.reconnect_timeout_ms = 10000, .timeout_ms = 15000},
        .buffer = {.size = 2048, .out_size = 6144},
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s:%u as %s", st.host, st.port, client_id);
}

static void cloud_task(void *arg)
{
    int64_t last_pub = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_reconfig) {
            s_reconfig = false;
            client_stop();
            load_config();
        }
        gw_cloud_status_t st;
        gw_cloud_get_status(&st);
        gw_wifi_status_t ws;
        gw_wifi_get_status(&ws);
        /* 连上路由器后才启动，之后断线由 esp-mqtt 自己重连 */
        if (!s_client && st.configured && ws.connected) {
            client_start();
        }
        if (!st.connected) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if ((s_dirty && now - last_pub >= STATE_MIN_GAP_US) || now - last_pub >= STATE_PERIOD_US) {
            s_dirty = false;
            last_pub = now;
            publish_state();
        } else if (s_dirty) {
            vTaskDelay(pdMS_TO_TICKS(STATE_MIN_GAP_US / 1000));
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

esp_err_t gw_cloud_init(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_t_state, sizeof(s_t_state), TOPIC_PREFIX "/%s/state", s_id);
    snprintf(s_t_online, sizeof(s_t_online), TOPIC_PREFIX "/%s/online", s_id);
    snprintf(s_t_cmd, sizeof(s_t_cmd), TOPIC_PREFIX "/%s/cmd", s_id);
    snprintf(s_t_resp, sizeof(s_t_resp), TOPIC_PREFIX "/%s/resp", s_id);
    load_config();
    return xTaskCreate(cloud_task, "gw_cloud", 6144, NULL, 4, &s_task) == pdPASS ? ESP_OK : ESP_FAIL;
}

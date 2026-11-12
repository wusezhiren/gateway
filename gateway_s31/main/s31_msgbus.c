#include "s31_msgbus.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    s31_msg_callback_t callback;
    void *context;
} subscriber_t;

static const char *TAG = "s31_msgbus";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static subscriber_t s_subscribers[S31_MSG_MAX][CONFIG_GW_S31_MSGBUS_MAX_SUBSCRIBERS];

static void dispatch_task(void *arg)
{
    s31_message_t message;
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        subscriber_t local[CONFIG_GW_S31_MSGBUS_MAX_SUBSCRIBERS];
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            memcpy(local, s_subscribers[message.id], sizeof(local));
            xSemaphoreGive(s_lock);
        }
        for (size_t i = 0; i < CONFIG_GW_S31_MSGBUS_MAX_SUBSCRIBERS; ++i) {
            if (local[i].callback) local[i].callback(&message, local[i].context);
        }
    }
}

esp_err_t s31_msgbus_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(CONFIG_GW_S31_MSGBUS_QUEUE_LENGTH, sizeof(s31_message_t));
    s_lock = xSemaphoreCreateMutex();
    if (!s_queue || !s_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(dispatch_task, "s31_msgbus",
                                CONFIG_GW_S31_MSGBUS_TASK_STACK, NULL,
                                CONFIG_GW_S31_MSGBUS_TASK_PRIORITY, NULL,
                                CONFIG_GW_S31_MSGBUS_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready queue=%d subscribers=%d core=%d",
             CONFIG_GW_S31_MSGBUS_QUEUE_LENGTH,
             CONFIG_GW_S31_MSGBUS_MAX_SUBSCRIBERS,
             CONFIG_GW_S31_MSGBUS_TASK_CORE);
    return ESP_OK;
}

esp_err_t s31_msgbus_subscribe(s31_msg_id_t id, s31_msg_callback_t callback, void *context)
{
    if (!s_queue || id <= S31_MSG_NONE || id >= S31_MSG_MAX || !callback) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (size_t i = 0; i < CONFIG_GW_S31_MSGBUS_MAX_SUBSCRIBERS; ++i) {
        if (!s_subscribers[id][i].callback) {
            s_subscribers[id][i] = (subscriber_t){callback, context};
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t s31_msgbus_unsubscribe(s31_msg_id_t id, s31_msg_callback_t callback, void *context)
{
    if (!s_queue || id <= S31_MSG_NONE || id >= S31_MSG_MAX || !callback) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (size_t i = 0; i < CONFIG_GW_S31_MSGBUS_MAX_SUBSCRIBERS; ++i) {
        if (s_subscribers[id][i].callback == callback && s_subscribers[id][i].context == context) {
            s_subscribers[id][i] = (subscriber_t){0};
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t s31_msgbus_publish(const s31_message_t *message, TickType_t timeout)
{
    if (!s_queue || !message || message->id <= S31_MSG_NONE || message->id >= S31_MSG_MAX ||
        message->length > CONFIG_GW_S31_MSGBUS_PAYLOAD_SIZE) return ESP_ERR_INVALID_ARG;
    return xQueueSend(s_queue, message, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

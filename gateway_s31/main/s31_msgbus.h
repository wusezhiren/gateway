#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    S31_MSG_NONE = 0,
    S31_MSG_SYSTEM_READY,
    S31_MSG_KEY,
    S31_MSG_TOUCH,
    S31_MSG_AUDIO_PCM,
    S31_MSG_WAKE,
    S31_MSG_VOICE_STATE,
    S31_MSG_DISPLAY,
    S31_MSG_CLOUD,
    S31_MSG_NODE,
    S31_MSG_MAX,
} s31_msg_id_t;

typedef struct {
    s31_msg_id_t id;
    uint32_t source;
    uint32_t flags;
    size_t length;
    union {
        struct { int key; bool pressed; } key;
        struct { int x; int y; bool pressed; } touch;
        struct { int state; uint32_t generation; } voice;
        struct { const void *data; size_t length; } buffer;
        uint8_t bytes[CONFIG_GW_S31_MSGBUS_PAYLOAD_SIZE];
    } data;
} s31_message_t;

typedef void (*s31_msg_callback_t)(const s31_message_t *message, void *context);

esp_err_t s31_msgbus_init(void);
esp_err_t s31_msgbus_subscribe(s31_msg_id_t id, s31_msg_callback_t callback, void *context);
esp_err_t s31_msgbus_unsubscribe(s31_msg_id_t id, s31_msg_callback_t callback, void *context);
esp_err_t s31_msgbus_publish(const s31_message_t *message, TickType_t timeout);

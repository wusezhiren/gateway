#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "meshproto.h"

static const char *TAG = "meshproto";

const uint8_t MP_BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void mp_hdr_init(mp_hdr_t *hdr, mp_type_t type, uint16_t seq)
{
    hdr->magic = MP_MAGIC;
    hdr->version = MP_VERSION;
    hdr->type = type;
    hdr->reserved = 0;
    hdr->seq = seq;
}

static int min_len(int type)
{
    switch (type) {
    case MP_JOIN_REQ:  return sizeof(mp_join_req_t);
    case MP_JOIN_RSP:  return sizeof(mp_join_rsp_t);
    case MP_HEARTBEAT:
    case MP_STATE:     return sizeof(mp_state_t);
    case MP_HB_ACK:    return sizeof(mp_hb_ack_t);
    case MP_SET:       return sizeof(mp_set_t);
    case MP_LEAVE:     return sizeof(mp_leave_t);
    case MP_OTA_BEGIN: return sizeof(mp_ota_begin_t);
    case MP_OTA_DATA:  return MP_OTA_DATA_HDR_LEN + 1;
    case MP_OTA_ACK:   return sizeof(mp_ota_ack_t);
    case MP_OTA_END:   return sizeof(mp_ota_end_t);
    case MP_OTA_ABORT: return sizeof(mp_ota_abort_t);
    default:           return -1;
    }
}

int mp_parse(const uint8_t *data, int len)
{
    if (len < (int)sizeof(mp_hdr_t)) {
        return 0;
    }
    const mp_hdr_t *hdr = (const mp_hdr_t *)data;
    if (hdr->magic != MP_MAGIC || hdr->version != MP_VERSION) {
        return 0;
    }
    int need = min_len(hdr->type);
    if (need < 0 || len < need) {
        return 0;
    }
    return hdr->type;
}

esp_err_t mp_espnow_setup(void)
{
#if CONFIG_MP_ENCRYPT
    if (strlen(CONFIG_MP_PMK) != ESP_NOW_KEY_LEN || strlen(CONFIG_MP_LMK) != ESP_NOW_KEY_LEN) {
        ESP_LOGE(TAG, "MP_PMK / MP_LMK must be exactly %d chars", ESP_NOW_KEY_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    return esp_now_set_pmk((const uint8_t *)CONFIG_MP_PMK);
#else
    return ESP_OK;
#endif
}

esp_err_t mp_peer_upsert(const uint8_t mac[6], bool encrypt)
{
    esp_now_peer_info_t peer = {
        .channel = 0,           /* 0 = 跟随当前信道 */
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, 6);
#if CONFIG_MP_ENCRYPT
    if (encrypt) {
        peer.encrypt = true;
        memcpy(peer.lmk, CONFIG_MP_LMK, ESP_NOW_KEY_LEN);
    }
#endif
    if (esp_now_is_peer_exist(mac)) {
        return esp_now_mod_peer(&peer);
    }
    return esp_now_add_peer(&peer);
}

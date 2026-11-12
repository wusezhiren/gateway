#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "meshproto.h"

static esp_now_peer_info_t last_peer;
static bool exists;
static int add_calls, mod_calls;
bool esp_now_is_peer_exist(const uint8_t *mac) { (void)mac; return exists; }
esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer) { last_peer = *peer; ++mod_calls; return ESP_OK; }
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer) { last_peer = *peer; ++add_calls; return ESP_OK; }
esp_err_t esp_now_set_pmk(const uint8_t *pmk) { (void)pmk; return ESP_OK; }

int main(void)
{
    mp_hdr_t hdr;
    mp_hdr_init(&hdr, MP_SET, 42);
    assert(hdr.magic == MP_MAGIC && hdr.version == MP_VERSION && hdr.seq == 42);
    assert(hdr.reserved == 0);
    assert(mp_parse(NULL, 0) == 0);
    assert(mp_parse((uint8_t *)&hdr, sizeof(hdr) - 1) == 0);
    assert(mp_parse((uint8_t *)&hdr, sizeof(hdr)) == 0); /* truncated SET */
    mp_set_t set = {0};
    set.hdr = hdr;
    assert(mp_parse((uint8_t *)&set, sizeof(set)) == MP_SET);
    set.hdr.magic = 0;
    assert(mp_parse((uint8_t *)&set, sizeof(set)) == 0);
    set.hdr.magic = MP_MAGIC;
    set.hdr.version++;
    assert(mp_parse((uint8_t *)&set, sizeof(set)) == 0);
    set.hdr.version = MP_VERSION;
    set.hdr.type = 255;
    assert(mp_parse((uint8_t *)&set, sizeof(set)) == 0);
    mp_join_req_t join = {0};
    mp_hdr_init(&join.hdr, MP_JOIN_REQ, 1);
    assert(mp_parse((uint8_t *)&join, MP_JOIN_REQ_MIN_LEN) == MP_JOIN_REQ);
    assert(!MP_JOIN_REQ_HAS(MP_JOIN_REQ_MIN_LEN, lp_window_ms));
    assert(MP_JOIN_REQ_HAS(sizeof(join), lp_window_ms));
    uint16_t rgb = MP_RGB_PACK(1, 67, MP_RGB_WHITE);
    assert((rgb & MP_RGB_ON) && MP_RGB_BRI(rgb) == 67 && MP_RGB_COLOR(rgb) == MP_RGB_WHITE);
    uint8_t mac[6] = {1,2,3,4,5,6};
    assert(mp_peer_upsert(mac, true) == ESP_OK && add_calls == 1);
    assert(memcmp(last_peer.peer_addr, mac, 6) == 0 && last_peer.channel == 0);
    exists = true;
    assert(mp_peer_upsert(mac, false) == ESP_OK && mod_calls == 1);
    puts("meshproto tests passed");
    return 0;
}

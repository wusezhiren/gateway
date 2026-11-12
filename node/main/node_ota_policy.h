#pragma once
#include <stdbool.h>
#include <stdint.h>

int node_ota_version_cmp(const char *a, const char *b);
bool node_ota_accept_chunk(uint32_t received, uint32_t size, uint32_t offset,
                           uint16_t length, uint16_t limit, int frame_len, int header_len);

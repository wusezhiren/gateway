#include <stdio.h>
#include "node_ota_policy.h"

int node_ota_version_cmp(const char *a, const char *b)
{
    if (!a || !b) return 0;
    int va[3] = {0}, vb[3] = {0};
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

bool node_ota_accept_chunk(uint32_t received, uint32_t size, uint32_t offset,
                           uint16_t length, uint16_t limit, int frame_len, int header_len)
{
    return length && length <= limit && frame_len >= header_len &&
           length <= frame_len - header_len && offset == received &&
           received <= size && length <= size - received;
}

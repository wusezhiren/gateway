#include <stdio.h>
#include "gw_ota_policy.h"

int gw_ota_version_cmp(const char *a, const char *b)
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

#include <assert.h>
#include <stdio.h>
#include "gw_ota_policy.h"

int main(void)
{
    assert(gw_ota_version_cmp("0.7.1", "0.7.0") > 0);
    assert(gw_ota_version_cmp("0.7.0", "0.7.1") < 0);
    assert(gw_ota_version_cmp("0.7.0", "0.7.0") == 0);
    assert(gw_ota_version_cmp("1.2", "1.2.0") == 0);
    assert(gw_ota_version_cmp(NULL, "1.0.0") == 0);
    puts("gateway OTA policy tests passed");
    return 0;
}

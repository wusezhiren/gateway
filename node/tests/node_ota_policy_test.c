#include <assert.h>
#include <stdio.h>
#include "node_ota_policy.h"

int main(void)
{
    assert(node_ota_version_cmp("0.4.2", "0.4.1") > 0);
    assert(node_ota_version_cmp("1.0.0", "1.0.1") < 0);
    assert(node_ota_version_cmp("1.2", "1.2.0") == 0);
    assert(node_ota_version_cmp("0.4.1", "0.4.1") == 0);
    assert(node_ota_accept_chunk(100, 200, 100, 50, 192, 70, 20));
    assert(!node_ota_accept_chunk(100, 200, 99, 50, 192, 70, 20));
    assert(!node_ota_accept_chunk(100, 200, 101, 50, 192, 70, 20));
    assert(!node_ota_accept_chunk(100, 200, 100, 0, 192, 70, 20));
    assert(!node_ota_accept_chunk(100, 200, 100, 193, 192, 213, 20));
    assert(!node_ota_accept_chunk(100, 200, 100, 51, 192, 70, 20));
    assert(!node_ota_accept_chunk(190, 200, 190, 11, 192, 31, 20));
    assert(!node_ota_accept_chunk(201, 200, 201, 1, 192, 21, 20));
    assert(!node_ota_accept_chunk(0, 200, 0, 1, 192, 19, 20));
    puts("node OTA policy tests passed");
    return 0;
}

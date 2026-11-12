#include <assert.h>
#include <stdio.h>
#include "gw_set_policy.h"

int main(void)
{
    assert(gw_set_retry_interval_us(0, 0) == 300000);
    assert(gw_set_retry_limit(0, 0) == 3);
    assert(gw_set_retry_interval_us(500, 50) == 25000);
    assert(gw_set_retry_limit(500, 50) == 42);
    assert(gw_set_retry_interval_us(500, 1) == 10000);
    assert(gw_set_retry_limit(500, 0) == 3);
    assert(gw_set_retry_limit(65535, 1) > 0); /* no 32-bit overflow */
    assert(gw_set_confirmed(0, 0) && gw_set_confirmed(1, 1));
    assert(!gw_set_confirmed(1, 0) && !gw_set_confirmed(0, 1));
    puts("SET policy tests passed");
    return 0;
}

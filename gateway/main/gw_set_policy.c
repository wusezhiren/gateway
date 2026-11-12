#include "gw_set_policy.h"

int64_t gw_set_retry_interval_us(uint16_t interval, uint8_t window)
{
    if (!interval || !window) return 300000;
    int half = window / 2;
    return (half < 10 ? 10 : half) * 1000LL;
}

int gw_set_retry_limit(uint16_t interval, uint8_t window)
{
    if (!interval || !window) return 3;
    return (int)(interval * 2000LL / gw_set_retry_interval_us(interval, window) + 2);
}

bool gw_set_confirmed(uint16_t actual, uint16_t requested)
{
    return actual == requested;
}

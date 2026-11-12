#pragma once
#include <stdbool.h>
#include <stdint.h>

int64_t gw_set_retry_interval_us(uint16_t lp_interval_ms, uint8_t lp_window_ms);
int gw_set_retry_limit(uint16_t lp_interval_ms, uint8_t lp_window_ms);
bool gw_set_confirmed(uint16_t actual, uint16_t requested);

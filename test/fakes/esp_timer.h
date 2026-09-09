#pragma once
/* Host fake: time stands still unless a test moves it. Deliberately NOT the
 * wall clock -- the code under test does overflow-safe arithmetic on this
 * value, and a test must be able to put it anywhere, including just below the
 * 32-bit millisecond wrap. */
#include <stdint.h>

extern int64_t fake_now_us;                 /* set by tests */
static inline int64_t esp_timer_get_time(void) { return fake_now_us; }
static inline void fake_time_set_ms(int64_t ms) { fake_now_us = ms * 1000; }
static inline void fake_time_advance_ms(int64_t ms) { fake_now_us += ms * 1000; }

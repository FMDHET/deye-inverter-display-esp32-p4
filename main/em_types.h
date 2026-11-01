#pragma once

/* Shared vocabulary of the energy manager: a time series of quarter hours.
 *
 * Every series (prices, PV forecast, load profile, the plan) is aligned to
 * the same grid: slot i covers [t0 + i*EM_SLOT_S, t0 + (i+1)*EM_SLOT_S), t0 is
 * a unix time that is a multiple of EM_SLOT_S (quarter hours are aligned in
 * UTC exactly as in local time, DST shifts by whole hours). 192 slots are 48 h,
 * enough for today + tomorrow from Tibber and two days of forecast.
 *
 * A value of NAN means "unknown" -- a consumer must treat it as missing, never
 * as zero. */

#include <math.h>
#include <stdint.h>

#define EM_SLOT_S     900
#define EM_MAX_SLOTS  192

typedef struct {
    int64_t t0;                  /* unix seconds of slot 0, multiple of EM_SLOT_S */
    uint16_t n;                  /* valid slots, 0..EM_MAX_SLOTS                  */
    float    v[EM_MAX_SLOTS];    /* value per slot, NAN = unknown                 */
} em_series_t;

/* Index of the slot containing unix time t, or -1 when outside the series. */
static inline int em_series_index(const em_series_t *s, int64_t t)
{
    if (!s || s->n == 0 || t < s->t0) return -1;
    int64_t i = (t - s->t0) / EM_SLOT_S;
    return i < s->n ? (int)i : -1;
}

/* Value at unix time t, NAN when unknown. */
static inline float em_series_at(const em_series_t *s, int64_t t)
{
    int i = em_series_index(s, t);
    return i < 0 ? NAN : s->v[i];
}

/* Round a unix time down to its quarter hour. */
static inline int64_t em_slot_floor(int64_t t) { return t - (((t % EM_SLOT_S) + EM_SLOT_S) % EM_SLOT_S); }

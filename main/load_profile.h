#pragma once

/* Household load profile: what the house draws in each quarter hour of the
 * day, learned from the live balance and used as the consumption forecast of
 * the energy manager.
 *
 * Two profiles of 96 quarter hours each, one for weekdays (Mon-Fri) and one
 * for weekends (Sat, Sun), indexed by LOCAL time. Every measured day goes
 * into a long-term record in flash (load_hist.h); the profile is the trimmed
 * average of the last ~6 weeks of each day type from there. Until a day type
 * has LH_MIN_DAYS stored days, an exponential moving average of the slot
 * means (kept in NVS) stands in -- also for slots the record has not seen.
 *
 * No task of its own: the energy manager task calls load_profile_tick()
 * every ~10 s. The getters copy under a spinlock and may be called from any
 * task. Every function takes the unix time from its caller, so the host tests
 * can drive the clock. */

#include <stdbool.h>
#include <stdint.h>
#include "em_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LP_SLOTS  96     /* quarter hours per day */

/* Load the stored profile from NVS. Call once before the first tick. */
void load_profile_init(void);

/* Feed one sample (modbus_tcp house power) at unix time `now`. Closes the
 * previous quarter hour when `now` lies in a new one, and saves the profile
 * at most once per hour. */
void load_profile_tick(int64_t now);

/* Forecast in kW for n slots starting at t0 (rounded down to a quarter hour),
 * n clamped to EM_MAX_SLOTS. Each slot uses the profile of its own local day
 * type; a slot never learned is NAN. */
void load_profile_forecast(int64_t t0, uint16_t n, em_series_t *out);

/* Write the profile now if it changed since the last save (shutdown hook,
 * before an OTA restart). Blocks on the NVS write. */
void load_profile_save_now(void);

typedef struct {
    float    wd_kw[LP_SLOTS];   /* weekday profile, kW, NAN = never learned */
    float    we_kw[LP_SLOTS];   /* weekend profile                          */
    uint16_t days_wd, days_we;  /* moving average: days learned with >= 80 % of their slots */
    /* The long-term record (load_hist.h): which source each profile is
     * (1 = average of stored days, 0 = moving average), how many days went
     * into the average, what the record holds, and today so far. */
    uint8_t  src[2];            /* [0] weekday, [1] weekend                 */
    uint16_t avg_days[2];
    uint16_t hist_days;
    int32_t  hist_oldest;       /* load_hist day key, 0 = none              */
    bool     hist_ok;           /* the flash area is there                  */
    int32_t  today_day;         /* day key of today_kw, 0 = none            */
    float    today_kw[LP_SLOTS];/* today's measured quarter hours, NAN = not (yet) */
} load_profile_t;

/* Snapshot of both profiles, safe from any task. */
void load_profile_get(load_profile_t *out);

#ifdef __cplusplus
}
#endif

#pragma once

/* Long-term record of the household load: one record per local day with the
 * mean power of every quarter hour, kept in flash for years, and the average
 * profile the energy manager plans with.
 *
 * Storage: a ring of 256-byte day records in a 1 MB flash area of its own
 * (label "loadhist", data subtype 0x40, at 0xA00000). Devices updated only
 * over the air still have the old partition table, which leaves that space
 * unused -- the module then registers the area itself at runtime. A record is
 * written once per day, at midnight; the ring overwrites the oldest 16 days
 * when it wraps after ~11 years.
 *
 * The average: per quarter hour of the local day, separately for weekdays
 * (Mon-Fri) and weekends, over the most recent LH_AVG_WD / LH_AVG_WE days of
 * that type (about six weeks), as a trimmed mean -- a party, a holiday, a
 * week away do not bend the profile.
 *
 * Not thread-safe on its own: the energy manager task does all writes; the
 * getters of load_profile copy the averages under its lock. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LH_SLOTS     96         /* quarter hours of a local day (wall clock)  */
#define LH_NONE      0xFFFFu    /* slot not measured                          */
#define LH_AVG_WD    30         /* weekdays averaged (~6 weeks)               */
#define LH_AVG_WE    12         /* weekend days averaged (~6 weeks)           */
#define LH_MIN_DAYS  3          /* fewer: the average is not used yet         */

typedef struct {
    int32_t  day;               /* days since 1970-01-01 of the LOCAL date    */
    uint8_t  wday;              /* 0 = Sunday .. 6 = Saturday                 */
    uint16_t w[LH_SLOTS];       /* mean W per quarter hour, LH_NONE = unknown */
} lh_day_t;

/* Find or register the flash area and find the newest record. Safe to call
 * again; returns the error of the flash layer if there is no area. */
esp_err_t load_hist_init(void);

/* Append a finished day. A day already stored is replaced in the averages
 * by the newer record (the older one stays in flash, unread). */
esp_err_t load_hist_put(const lh_day_t *d);

/* Number of days stored, and the oldest stored date (day key, 0 = none). */
int       load_hist_count(int32_t *oldest_day);

/* The k-th newest stored day (k = 0: the newest). false past the end. */
bool      load_hist_get(int k, lh_day_t *out);

/* Average profile of a day type (0 weekday, 1 weekend) over the newest
 * `max_days` days of that type: W per slot, NAN where no day had a value.
 * *ndays = days that went in. */
void      load_hist_average(uint8_t we, int max_days, float out_w[LH_SLOTS], uint16_t *ndays);

/* The local date of a day key, for display. */
void      load_hist_date(int32_t day, int *y, int *m, int *d);

#ifdef __cplusplus
}
#endif

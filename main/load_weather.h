#pragma once

/* Household load against the weather: does the house use more on cold days
 * (heat pump, electric heating), and how much more tomorrow?
 *
 * The load profile (load_profile.h) knows weekday and time of day only. This
 * module fetches the daily mean temperature of the last LW_PAST_DAYS days and
 * the next three from Open-Meteo (one small request every 6 h, no key) and
 * fits, over the days in the long-term load record (load_hist.h),
 *     ratio = alpha + beta * HDD
 * where ratio = the day's energy / the profile's energy for its day type, and
 * HDD = max(0, LW_HDD_BASE - mean temperature). Only a clear relation is used
 * (enough days, beta > 0, r^2 >= LW_MIN_R2); a house without electric heating
 * fits beta ~ 0 and keeps factor 1. The forecast factor for a day scales
 * every quarter hour of that day's load forecast, clamped to
 * [LW_FACTOR_MIN, LW_FACTOR_MAX].
 *
 * The energy manager task calls load_weather_tick(); getters copy under a
 * spinlock. The fit itself is pure (lw_fit, host-tested). */

#include <stdbool.h>
#include <stdint.h>
#include "em_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LW_PAST_DAYS   60
#define LW_FC_DAYS     3
#define LW_HDD_BASE    15.0f   /* deg C: below this the heating runs          */
#define LW_MIN_DAYS    14
#define LW_MIN_R2      0.25f
#define LW_FACTOR_MIN  0.6f
#define LW_FACTOR_MAX  1.8f

typedef struct { float hdd, ratio; } lw_point_t;

typedef struct {
    bool  ok;             /* the relation is used                            */
    float alpha, beta;    /* ratio = alpha + beta * HDD                      */
    float r2;
    int   n;              /* days in the fit                                 */
} lw_model_t;

/* Least squares over n points. ok = false when the relation is too weak or
 * there are too few days; alpha/beta/r2/n are filled in anyway. */
void  lw_fit(const lw_point_t *p, int n, lw_model_t *m);
/* Factor for a day with mean temperature t (1 when !m->ok or t is NAN). */
float lw_factor(const lw_model_t *m, float t);

typedef struct {
    lw_model_t m;
    float      kwh_per_k;        /* beta in kWh per day and degree below base */
    int32_t    day[LW_FC_DAYS];  /* local day keys (load_hist), 0 = none      */
    float      t_mean[LW_FC_DAYS];
    float      factor[LW_FC_DAYS];
    int64_t    fetched;          /* unix time of the last good answer, 0 = never */
    char       err[64];
} load_weather_t;

void  load_weather_tick(int64_t now, const em_cfg_t *c);
void  load_weather_get(load_weather_t *out);
/* Factor for a local day key (1 when unknown). */
float load_weather_factor(int32_t day);
/* Local day key of a unix time, the same key load_hist uses. */
int32_t load_weather_day_key(int64_t t);

#ifdef __cplusplus
}
#endif

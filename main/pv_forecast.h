#pragma once

/* PV production forecast per PV plane, in quarter hours, for today and
 * tomorrow (local days), calibrated against what the plane really produced.
 *
 * Source: Open-Meteo (no key), global tilted irradiance and air temperature
 * for each plane's tilt and orientation. A simple PV model turns that into
 * AC kW; a per-plane factor learnt from the measured production of the last
 * days (meter assignment in em_cfg_t) absorbs everything the model does not
 * know: shading, soiling, inverter clipping, a wrong kWp entry.
 *
 * No task of its own: the energy manager task calls pv_forecast_tick() every
 * ~10 s; the tick integrates the measurement and, when due, fetches
 * (blocking, a few seconds per plane). */

#include <stdint.h>
#include "em_cfg.h"
#include "em_types.h"

/* System losses of the operator's model: 8 % (cabling, mismatch, inverter,
 * soiling) on top of the Huld module efficiency. The model itself is in
 * pv_forecast.c (Spencer sun, HDKR transposition, Martin-Ruiz IAM, Faiman
 * cell temperature, Huld efficiency, snow derate). */
#define PV_FC_PR          0.92f

/* Calibration: EMA weight per day, clamp, and the two conditions a day must
 * meet before it counts. */
#define PV_CAL_ALPHA      0.2f
#define PV_CAL_MIN        0.3f
#define PV_CAL_MAX        1.5f
#define PV_CAL_MIN_KWH    0.5f    /* group forecast of the day must exceed this */
#define PV_CAL_MIN_COVER  0.70f   /* share of that forecast that was measured   */

/* Schedule. */
#define PV_FC_PERIOD_S    3600
#define PV_FC_CLOCK_OK    1700000000LL   /* earlier = clock not set yet         */

/* Today's measurement per quarter hour: one local day, 25 h on the day the
 * clocks go back. */
#define PV_MEAS_SLOTS     100

typedef struct {
    char        name[16];
    em_series_t kw;             /* calibrated forecast, kW, NAN unknown */
    float       cal;            /* current factor                       */
    float       kwh_today, kwh_tomorrow;   /* calibrated forecast sums (local days), NAN unknown */
    float       meas_kwh_today;            /* measured so far today, NAN if not measured */
    /* Measured energy per quarter hour of today (slot k starts at
     * pv_fc_t.meas_t0 + 900 k), and how much of the slot the measurement
     * covered (0..100 %). NAN / 0 where nothing was measured -- the future,
     * a meter offline, the display off. */
    float       meas_kwh[PV_MEAS_SLOTS];
    uint8_t     meas_cov[PV_MEAS_SLOTS];
} pv_plane_fc_t;

typedef struct {
    uint8_t       n;
    pv_plane_fc_t p[EM_MAX_PLANES];
    em_series_t   total_kw;     /* sum of planes; NAN where any plane is unknown */
    int64_t       fetched;      /* unix time of the last good fetch, 0 = never   */
    int64_t       meas_t0;      /* local midnight of the measured day, 0 = none   */
    uint8_t       meas_n;       /* slots of that day: 92, 96 or 100              */
    char          err[80];      /* German, "" = none                    */
} pv_fc_t;

/* Called every ~10 s from the energy manager task with the unix time.
 * Integrates the measurement, rolls the day over (calibration), and fetches
 * when due -- blocking. */
void pv_forecast_tick(int64_t now);

/* Refetch at the next tick (settings changed, button on the web page).
 * Callable from any task. */
void pv_forecast_request(void);

/* Copy of the current forecast, taken under a spinlock; callable from any
 * task. pv_fc_t is ~8.9 kB: pass a static buffer, never one on the stack. */
void pv_forecast_get(pv_fc_t *out);

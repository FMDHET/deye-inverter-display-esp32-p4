#pragma once

/* Electricity prices per quarter hour and the German grid mix forecast, for
 * the energy manager.
 *
 *   Tibber (primary)     end price incl. all fees and taxes plus its exchange
 *                        part, today and -- from about 13:00 -- tomorrow.
 *   Energy-Charts        exchange price only (DE-LU day-ahead). Fallback when
 *                        Tibber is not configured or fails; the end price is
 *                        estimated as exchange * 1.19 + an offset learned from
 *                        the last good Tibber answer (20 ct before there was
 *                        one). Energy-Charts rate-limits hard: fetched rarely.
 *   Energy-Charts mix    renewable / solar / wind share forecast, today.
 *
 * Every series is aligned to local midnight of today (em_types.h). No task of
 * its own: the energy manager task calls price_feed_tick(), which does the
 * (blocking) HTTPS requests when they are due -- at most one per call. */

#include <stdbool.h>
#include <stdint.h>
#include "em_types.h"

typedef struct {
    em_series_t total;      /* ct/kWh end price, NAN unknown          */
    em_series_t energy;     /* ct/kWh exchange part, NAN unknown      */
    char        src[16];    /* "tibber", "energy-charts" or ""        */
    int64_t     fetched;    /* unix time of the last good fetch, 0    */
    bool        tomorrow;   /* prices for tomorrow are in             */
    char        err[80];    /* last error, German, "" = none          */
} price_set_t;

typedef struct {
    em_series_t ren, solar, wind_on, wind_off;   /* percent */
    int64_t     fetched;
    char        err[80];
    /* Generation forecast (Energy-Charts public_power_forecast, day-ahead),
     * GW. Fetched separately from the shares above, one type per request. */
    em_series_t wind_on_gw, wind_off_gw, solar_gw;
    int64_t     gen_fetched;
} grid_mix_t;

/* Called every ~10 s from the energy manager task with the wall clock (unix
 * seconds). Does the fetches when due, blocking, one request per call.
 * Does nothing while the clock is not set. */
void price_feed_tick(int64_t now);

/* Force a refresh of prices and mix at the next ticks (from the web/MQTT).
 * Error backoff and an HTTP 429 hold-off still apply. */
void price_feed_request(void);

/* Thread-safe copies, callable from any task (only copy under a spinlock). */
void price_feed_get(price_set_t *out);
void price_feed_get_mix(grid_mix_t *out);

/* State of each source, for the web page ("is it reachable?"). */
enum { PF_SRC_TIBBER, PF_SRC_EC_PRICE, PF_SRC_MIX, PF_SRC_GEN, PF_SRC_N };
typedef struct {
    bool    used;       /* false: not asked at all (no token, not needed as fallback) */
    int64_t last_try;   /* last attempt, 0 = never                                   */
    int64_t last_ok;    /* last good answer, 0 = never since start                    */
    int64_t next;       /* next attempt as scheduled, 0 = none planned                */
    uint8_t fails;      /* attempts in a row that failed                              */
    bool    r429;       /* the last failure was "too many requests"                   */
    bool    tomorrow;   /* the last good answer covered tomorrow                      */
    uint8_t stage;      /* generation: requests of the current round done (0..6)     */
    char    err[80];    /* last error, German, "" = none                              */
} pf_src_t;
void price_feed_sources(int64_t now, pf_src_t out[PF_SRC_N]);

/* The offset (ct/kWh) the Energy-Charts fallback adds to exchange * 1.19,
 * and whether it was learned from Tibber (false = the 20 ct default). */
float price_feed_offset_ct(bool *learned);

#pragma once

/* What the batteries (and the energy manager) actually saved, per day, from
 * the quarter-hour record (em_hist.h):
 *
 *   cost   what the grid really cost: import x price - export x feed-in
 *   base   the same day without batteries: the house's deficit (load - PV)
 *          bought at the price, its surplus exported at the feed-in tariff
 *   stored the change of the energy in both batteries over the day, valued
 *          at what it cost (bat_cost.h) -- charging for tomorrow is not a
 *          loss of today, emptying yesterday's charge not a gain of today
 *   saved  = base - (cost - stored)
 *
 * Feed-in: em_cfg feedin_ct; with feedin_neg0, nothing for slots with a
 * negative price. A slot that misses grid, PV, load or price is left out of
 * all three sums (slots tells how many were counted).
 *
 * Cache of the last EM_SAV_DAYS local days including today, recomputed by
 * the energy manager task (today every quarter hour, all days at midnight). */

#include <stdbool.h>
#include <stdint.h>
#include "em_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EM_SAV_DAYS 32

typedef struct {
    int64_t  t0;           /* local midnight, unix time; 0 = empty entry    */
    uint16_t slots;        /* quarter hours counted                         */
    float    cost_ct, base_ct, stored_ct;
    float    imp_kwh, exp_kwh, pv_kwh, house_kwh;
} em_sav_day_t;

void em_savings_tick(int64_t now, const em_cfg_t *c);
/* Oldest first; returns the number of days copied. */
int  em_savings_get(em_sav_day_t *out, int max);

#ifdef __cplusplus
}
#endif

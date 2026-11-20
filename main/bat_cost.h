#pragma once

/* Price of the energy sitting in the Deye and the BYD battery: what one kWh
 * coming out of each costs, from where it was charged (grid at the Tibber
 * price, PV at the feed-in tariff, the other battery at its own price),
 * losses included. The arithmetic and its rules are in bat_cost_calc.h.
 *
 * Runs on the energy manager's 10 s tick, in every mode -- it only watches.
 * Off while no feed-in tariff is set (em_cfg_t.feedin_ct = 0). The state
 * lives in NVS (namespace "em", key "bcost"), written every 15 min and on
 * every orderly restart; a power cut loses at most that quarter hour of
 * mixing, never the price itself. */

#include <stdbool.h>
#include <stdint.h>
#include "bat_cost_calc.h"
#include "em_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAT_COST_SAVE_MS  (15 * 60 * 1000)

typedef struct {
    bool       valid;        /* priced: capacity set and seen at least once  */
    float      out_ct;       /* ct per kWh it delivers (AC side)             */
    float      grid_pct;     /* share of the content from the grid, %        */
    float      content_kwh;  /* cell energy now (SoC x capacity), NAN unknown */
    float      value_eur;    /* what the content is worth when taken out     */
    float      eta_c, eta_d; /* efficiencies used                            */
    bool       eta_measured; /* the Deye's long-term measurement, not a default */
    bc_stats_t st;           /* charged since `since`, by source             */
} bat_cost_bat_t;

typedef struct {
    bool           active;     /* feed-in tariff set                         */
    float          feedin_ct;
    float          grid_ct;    /* grid price used in the last step           */
    bool           grid_live;  /* ... and it came from the price feed        */
    int64_t        since;      /* unix time accounting (re)started, 0 = not yet */
    bat_cost_bat_t deye, byd;
} bat_cost_status_t;

/* Load the state and register the save-on-restart handler. */
void bat_cost_init(void);

/* One step, from the energy manager task. price_now_ct = the end price of
 * this quarter hour, NAN when unknown (the last known one is kept). */
void bat_cost_tick(int64_t now, const em_cfg_t *c, float price_now_ct);

void bat_cost_get(bat_cost_status_t *out);

/* Write the state to NVS now if it changed (orderly restart). */
void bat_cost_save_now(void);

/* Forget the history: both batteries start again at the feed-in tariff
 * (applied on the next tick). */
void bat_cost_reset(void);

#ifdef __cplusplus
}
#endif

#pragma once

/* Energy manager -- the plan, without the I/O.
 *
 * Given quarter-hour prices (Tibber end prices), a PV forecast, a load
 * forecast and the battery as it is now, decide for every slot what the Deye
 * should do so the household buys as little expensive energy as possible:
 *
 *   NORMAL  self-consumption: PV surplus charges, a deficit discharges
 *   HOLD    the battery does not discharge (PV may still charge it)
 *   CHARGE  the battery charges from the grid, up to soc_max
 *
 * Why a greedy and not an optimiser: the result has to be explainable on a
 * display ("charging now at 21 ct for 18:30 at 38 ct"), it has to stay
 * bounded on a 400 MHz MCU, and every decision is checked against a full
 * simulation of the horizon -- so whatever the greedy proposes, it never
 * accepts a move the simulation says costs money. Each accepted move records
 * the expensive slot it was made for (for_slot), which is the explanation.
 *
 * Pure function: no ESP-IDF, no FreeRTOS, no allocation, no globals, no
 * logging. Float only (the P4 has a single-precision FPU; double is soft).
 * Stack use is about 2 KB on top of the two structs, which the caller owns
 * (em_plan_in_t ~2.4 KB, em_plan_t ~4.6 KB -- keep them off small task stacks).
 */

#include <stdint.h>

#include "em_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EM_ACT_NORMAL 0
#define EM_ACT_HOLD   1
#define EM_ACT_CHARGE 2

/* for_slot of a move made for the energy left after the horizon (the plan
 * keeps or buys energy because its end value is worth more than using it in
 * a cheap slot now). Negative, so `for_slot >= 0` still means "a slot". */
#define EM_FOR_END    (-2)

/* Upper bound on greedy passes (one pass = one expensive slot examined).
 * Bounds the run time on the device whatever the input looks like. */
#define EM_PLAN_MAX_ITER 400

/* Grid charging is planned in steps of this much AC energy per slot
 * (1 kW for a quarter hour). Several steps on the same slot add up, so a
 * slot can reach charge_kw; the loss model makes concentrating the charge in
 * the cheapest slots at higher power the cheaper choice by itself. */
#define EM_PLAN_CHG_QUANTUM_KWH 0.25f

/* Charge saturation: above this SoC the battery accepts linearly less, down
 * to EM_TAPER_END x charge_kw at 100 %. Applies to PV and grid alike. */
#define EM_TAPER_FROM_PCT 90.0f
#define EM_TAPER_END      0.2f

typedef struct {
    int64_t  t0;                       /* unix time of slot 0 (quarter hour) */
    uint16_t n;                        /* slots given; the horizon ends at the first slot whose price, pv or load is NAN */
    /* Slots after the priced horizon (from index n on) for which PV and load
     * are still known -- the rest of the weather forecast. No decisions are
     * made there; they tell what energy left at the end is worth: only what
     * it saves before PV would fill the battery anyway. 0 = value the end
     * flat at the median price. pv_kw/load_kw[n .. n+n_ext-1] must be set. */
    uint16_t n_ext;
    float    price_ct[EM_MAX_SLOTS];   /* end price ct/kWh                     */
    float    pv_kw[EM_MAX_SLOTS];      /* expected PV (AC) kW, whole house     */
    float    load_kw[EM_MAX_SLOTS];    /* expected house load kW               */
    float    soc_pct;                  /* battery now                          */
    float    cap_kwh;                  /* usable capacity (0 → plan nothing)   */
    float    soc_min_pct, soc_max_pct; /* discharge floor / grid-charge ceiling */
    float    charge_kw;                /* max grid-charge AC power             */
    float    dis_kw_max;               /* max discharge AC power               */
    float    min_spread_ct;            /* a decision must save at least this per kWh moved */
    float    cycle_cost_ct;            /* wear per kWh discharged (DC)         */
    float    feedin_ct;                /* paid per exported kWh (may be 0)     */
    float    pv_confidence;            /* 0..1, scales PV down for planning grid charges (e.g. 0.8) */
    float    loss_dis[3], loss_chg[3]; /* a,b,c; all zero → defaults above     */
} em_plan_in_t;

typedef struct {
    uint8_t  act;          /* EM_ACT_*                                       */
    float    charge_kw;    /* grid-charge AC power when act == CHARGE        */
    float    soc_end;      /* expected SoC at the end of the slot, %         */
    float    grid_kw;      /* expected grid power, + import / − export       */
    float    cost_ct;      /* expected cost of the slot                      */
    int16_t  for_slot;     /* CHARGE/HOLD: the expensive slot this serves, −1;
                              EM_FOR_END: the energy left after the horizon */
} em_slot_plan_t;

typedef struct {
    int64_t        t0;
    uint16_t       n;              /* planned slots (the horizon)            */
    em_slot_plan_t s[EM_MAX_SLOTS];
    float          cost_plan_ct;   /* incl. terminal value                   */
    float          cost_base_ct;   /* all NORMAL, same accounting            */
    float          end_value_ct;   /* value of the energy left at the end    */
    uint16_t       iterations;
} em_plan_t;

/* Loss model of the inverter, fitted to a real power sweep: a constant part
 * while it runs (~0.1 kW) and a quadratic rise at high power.
 *     loss_kW(P) = a + b*P + c*P^2       (P = AC power in kW)
 * Discharge: DC drawn  = P + loss.   Charge: DC stored = P - loss.
 * The constant part is why a 50 W fridge served from the battery costs three
 * times its energy, and why the planner prefers few strong charge slots. */
#define EM_LOSS_DIS_A 0.09f
#define EM_LOSS_DIS_B 0.0f
#define EM_LOSS_DIS_C 0.004f
#define EM_LOSS_CHG_A 0.11f
#define EM_LOSS_CHG_B 0.0f
#define EM_LOSS_CHG_C 0.006f

/* Plan the horizon. `out` is overwritten completely; slots beyond out->n are
 * zero with for_slot = -1.
 *
 * Accounting, the same for cost_plan_ct and cost_base_ct:
 *     sum(import * price) - sum(export * feedin) + cycle_cost * DC discharged
 *     - end_value
 * end_value is what the energy left above soc_min is worth after the horizon:
 * DC kWh * (eta_dis(1 kW) * median price - cycle_cost), never negative.
 * Without it the plan would drain the battery at the last slot and never
 * prepare for anything it cannot see.
 *
 * pv_confidence scales the PV forecast for the WHOLE plan -- the moves are
 * judged and the numbers are reported in that one scenario. Judging grid
 * charges against 80 % PV but holds and costs against 100 % would compare
 * moves on two different scales, and cost_plan could no longer be compared
 * with cost_base. With 1.0 the plan is the plain forecast; <= 0 or NAN is
 * read as 1.0 (an unset field must not make the planner ignore the sun).
 *
 * cost_plan_ct <= cost_base_ct always holds: the plan starts as the base and
 * only accepts moves that make it cheaper.
 *
 * Unknown battery (cap_kwh <= 0, soc NAN): no moves, the slots still carry
 * the grid power and cost of PV + load alone. */
void em_plan_compute(const em_plan_in_t *in, em_plan_t *out);

#ifdef __cplusplus
}
#endif

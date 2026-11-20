#pragma once

/* The BYD's share of the plan: keep its energy for the dearest hours.
 *
 * The BYD behind the Fronius discharges into whatever the house lacks, the
 * moment it lacks it -- on a cheap afternoon as readily as at the evening
 * peak. The display can only block its discharge (fronius_storage), not set
 * a power. So the plan is a set of slots in which it is blocked:
 *
 *   - the energy above the reserve, as AC: (SoC - reserve) x capacity x eta
 *   - the slots with a deficit (load > PV) up to the next refill by the sun,
 *     each needing at most p_max x 15 min
 *   - the dearest of those get the energy, until it is spent; the other
 *     deficit slots are blocked. Slots without a deficit are never blocked
 *     (nothing would discharge, and PV may charge).
 *
 * "Refill": the first surplus slot from which the PV surplus of the next two hours
 * adds up to REFILL_KWH -- after it the battery fills again, energy kept past
 * that point is worth nothing. Nothing after it is blocked.
 *
 * The BYD's real discharge is smaller than the whole deficit (the Deye
 * covers part of it), so the energy lasts longer than planned: the plan
 * errs towards keeping a little, never towards running dry early.
 *
 * Pure, host-tested (test/test_em_byd_plan.c). */

#include <math.h>
#include <stdint.h>

#define BYD_PLAN_REFILL_KWH 1.0f

/* block[i] = 1: keep the BYD from discharging in slot i. Returns the price
 * (ct) of the cheapest slot that still gets BYD energy, NAN when every
 * deficit slot is served (nothing blocked) or there is no energy at all. */
static inline float em_byd_plan(const float *price, const float *pv, const float *load, int n,
                                float e_avail_kwh, float p_max_kw, uint8_t *block)
{
    int end = n;
    for (int i = 0; i < n; i++) {
        block[i] = 0;
    }
    /* the next refill */
    for (int i = 0; i < n; i++) {
        if (pv[i] - load[i] <= 0) continue;      /* the run starts with a surplus slot */
        float sur = 0;
        for (int k = i; k < n && k < i + 8; k++) {
            float s = pv[k] - load[k];
            if (s > 0) sur += s * 0.25f;
        }
        if (sur >= BYD_PLAN_REFILL_KWH) { end = i; break; }
    }
    if (!(e_avail_kwh > 0)) {
        /* nothing to keep: block nothing, the reserve guards the rest */
        return NAN;
    }
    /* serve the dearest deficit slots first */
    float left = e_avail_kwh, served_min = NAN;
    uint8_t done[256] = { 0 };
    int lim = end < 256 ? end : 256;
    for (;;) {
        int best = -1;
        for (int i = 0; i < lim; i++) {
            if (done[i] || !isfinite(price[i])) continue;
            if (load[i] - pv[i] <= 0.05f) continue;
            if (best < 0 || price[i] > price[best]) best = i;
        }
        if (best < 0) break;
        done[best] = 1;
        if (left <= 0) { block[best] = 1; continue; }
        float d = load[best] - pv[best];
        if (d > p_max_kw) d = p_max_kw;
        left -= d * 0.25f;
        served_min = price[best];
    }
    for (int i = 0; i < lim; i++) if (block[i]) return served_min;
    return NAN;
}

#pragma once

/* Lifetime energy counters for Home Assistant (total_increasing): the Eltako
 * meters' import/export, the Deye's battery charge/discharge, and the BYD sums
 * the display integrates itself. Pure, host-tested (test_energy_ctr.c).
 *
 * Why the care: HA reads any decrease of a total_increasing sensor as a meter
 * reset and any jump as real energy. One garbled Modbus read would put a
 * spike of thousands of kWh into the long-term statistics, and those are not
 * corrected by the next good value. */

#include <stdbool.h>
#include <stdint.h>

/* Fastest a counter may move: far above any battery or house connection
 * here (35 A x 3 phases ~ 24 kW), far below what a garbled register gives. */
#define ENERGY_CTR_MAX_W        100000.0
/* Implausible readings in a row that are taken as real after all: the meter
 * was swapped or reset, and refusing it forever would freeze the sensor. */
#define ENERGY_CTR_MAX_REJECTS  3

typedef struct {
    bool     valid;       /* a value has been accepted                    */
    uint32_t val;         /* raw counter, in the device's units           */
    uint32_t ms;          /* time of the last accepted value (ms)         */
    uint8_t  rejects;     /* implausible readings since then              */
} energy_ctr_t;

/* `now` may follow `prev` only forward, and by no more than ENERGY_CTR_MAX_W
 * over dt_ms allows, plus 0.1 kWh slack for rounding and read jitter.
 * units_per_kwh: 100 for 0.01 kWh registers, 10 for 0.1 kWh. */
static inline bool energy_ctr_step_ok(uint32_t prev, uint32_t now, uint32_t dt_ms,
                                      double units_per_kwh)
{
    double max_step = (ENERGY_CTR_MAX_W / 1000.0 * dt_ms / 3600000.0 + 0.1) * units_per_kwh;
    return now >= prev && (double)(now - prev) <= max_step;
}

/* Feed one fresh reading. True when it was accepted into c->val. */
static inline bool energy_ctr_feed(energy_ctr_t *c, uint32_t raw, uint32_t now_ms,
                                   double units_per_kwh)
{
    bool ok = !c->valid ||
              energy_ctr_step_ok(c->val, raw, (uint32_t)(now_ms - c->ms), units_per_kwh) ||
              c->rejects + 1 >= ENERGY_CTR_MAX_REJECTS;
    if (ok) {
        c->valid = true; c->val = raw; c->ms = now_ms; c->rejects = 0;
    } else {
        c->rejects++;
    }
    return ok;
}

/* Integrate one battery power sample (W, + = discharge / - = charge) held for
 * dt_ms into the two Wh sums. Both sums only ever grow. */
static inline void energy_integrate(double *chg_wh, double *dis_wh, float w, uint32_t dt_ms)
{
    double wh = (double)w * dt_ms / 3600000.0;
    if (wh > 0) *dis_wh += wh;
    else        *chg_wh -= wh;
}

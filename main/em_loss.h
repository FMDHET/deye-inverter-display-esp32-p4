#pragma once

/* The Deye's measured loss model, for the planner.
 *
 * The efficiency curve (modbus_rtu: energy in/out per AC power bin, both
 * directions) is fitted to
 *     loss_kW(P) = a + b*P + c*P^2          (P = AC power in kW)
 * by weighted least squares -- the same fit the /deye page draws as the
 * "Verlustmodell" (deye.html, fit()), so the planner and the chart agree:
 * weight = seconds measured, capped at one hour; a and c are physically
 * non-negative (own consumption, I^2*R), a negative one is dropped and the
 * rest refitted. Fewer than three bins with 10 s, or a power range narrower
 * than 1:3, give no model (false) and the planner keeps its defaults.
 *
 * Pure: no ESP-IDF, host-tested (test/test_em_loss.c). */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float    in_wh, out_wh;
    uint32_t s;
} em_loss_bin_t;               /* same layout as mb_eff_bin_t */

/* dis = true: discharge bins (AC is the output), false: charge (AC input).
 * out = {a, b, c} in kW units as above. */
bool em_loss_fit(const em_loss_bin_t *bins, int n, bool dis, float out[3]);

#pragma once

/* Charged / discharged energy of the BYD battery behind the Fronius Symo
 * Hybrid, for Home Assistant's energy dashboard.
 *
 * The Symo Hybrid has no such counters: SunSpec model 160 marks DCWH as not
 * implemented (scale factor 0x8000, value 0) and the Solar API's storage data
 * carries SoC, voltage and temperature only (checked 2026-10-25). So the
 * display integrates the BYD power itself -- the same value the energy
 * balance uses (AC - solar DC of the hybrid, + = discharge), i.e. the
 * battery's share on the AC side, conversion losses included.
 *
 * The sums live in NVS (written every BYD_ENERGY_SAVE_MS and on every orderly
 * restart), so they keep counting across updates. A power cut loses at most
 * the last quarter hour, which HA simply never sees -- the sum does not go
 * backwards, it just misses that piece. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BYD_ENERGY_SAVE_MS  (15 * 60 * 1000)

/* Load the sums from NVS and register the save-on-restart handler. */
void byd_energy_init(void);

/* One fresh BYD power reading (W, + = discharge) from device slot `slot`.
 * A gap longer than max_gap_ms since the previous sample is skipped rather
 * than filled with this value. Only the first slot that reports is counted:
 * two hybrids would otherwise interleave into one sum. */
void byd_energy_sample(int slot, float w, uint32_t now_ms, uint32_t max_gap_ms);

/* Totals in kWh. False while there is nothing to report (never sampled and
 * nothing stored). */
bool byd_energy_get(double *chg_kwh, double *dis_kwh);

#ifdef __cplusplus
}
#endif

#pragma once

/* Which modules of a SunSpec MPPT model (160) carry PV.
 *
 * Fronius hybrids report their battery DC port in the same model, as if it
 * were another string -- and DCW there has no sign, so charging and
 * discharging both look like production. Summing every module as "solar"
 * (what modbus_tcp did until 2026-10-24) counted a discharging BYD as PV at
 * night: "PV 1993 W, Akku -78 W" on the Symo Hybrid while it was dark.
 *
 *  - Fronius GEN24 and later name the storage modules "StCha n" and
 *    "StDisCha n": those are not PV.
 *  - Symo Hybrid (storage model 124 present, no such names): the battery is
 *    the LAST DC input (here "String 2", ~310 V with the BYD HV).
 *  - Anything else: every module is PV. */

#include <stdbool.h>
#include <stdint.h>

#define SUNSPEC_MPPT_MAX 16

/* ids[i]: the module's IDStr, NUL-terminated (up to 16 chars). hybrid: the
 * device carries storage model 124. Returns bit i set = module i is PV. */
uint16_t sunspec_mppt_pv_mask(const char ids[][17], uint16_t n, bool hybrid);

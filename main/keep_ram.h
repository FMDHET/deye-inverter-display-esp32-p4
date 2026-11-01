#pragma once

/* Memory that survives a software restart (an OTA, a panic, a watchdog), for
 * the measurements of the running day: PV per quarter hour (pv_forecast.c)
 * and household load per quarter hour (load_profile.c).
 *
 * It lives in the P4's LP RAM (RTC_NOINIT_ATTR) as ONE object with fixed
 * offsets. The first attempt used EXT_RAM_NOINIT_ATTR in PSRAM: that segment
 * sits behind .ext_ram.bss and moved with every change of a PSRAM buffer,
 * and the linker also swapped the two areas between builds 361 and 362 --
 * the new firmware then looked in the wrong place and the day's curve
 * restarted. LP RAM holds next to nothing else (a little IDF code ahead of
 * it), so the address stays put from build to build. A power cut clears it;
 * the users check magic, size, checksum and date before trusting anything. */

#include <stdint.h>

#define KEEP_PV_BYTES  4096
#define KEEP_LP_BYTES  1024

typedef struct {
    uint64_t pv[KEEP_PV_BYTES / 8];      /* pv_forecast.c: pv_keep_t       */
    uint64_t lp[KEEP_LP_BYTES / 8];      /* load_profile.c: lp_keep_t      */
} keep_ram_t;

extern keep_ram_t g_keep_ram;

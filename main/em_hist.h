#pragma once

/* What actually happened, per quarter hour: the Deye's SoC at the end of the
 * slot, the BYD's, the mean powers, the price, and what the energy manager
 * had planned (and whether it carried it out). The page draws it in the past
 * part of "Preise & Plan" -- the plan itself always starts at the current
 * quarter hour, so without this record the morning's SoC curve and the
 * charge blocks already served were gone from the chart.
 *
 * Storage: an append-only ring of 32-byte records in a 1 MB flash area of
 * its own (label "emhist", data subtype 0x41, at 0xB00000; registered at
 * runtime on devices that only ever got OTA, like load_hist). One record per
 * quarter hour, written when the slot ends: ~340 days fit, a 4 kB sector is
 * erased every ~32 h. Nothing is kept in RAM across a restart -- the slot
 * running at the time is lost, every slot before it is in flash.
 *
 * The energy manager task calls em_hist_tick() every ~10 s; the web server
 * reads with em_hist_range(). One lock around both. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EH_ACT_NONE   0xFF      /* no plan for that slot                     */

#define EH_F_ACTING   0x01      /* AUTO carried the plan out (most of the slot) */
#define EH_F_PAUSED   0x02      /* the user had taken over                   */

/* One tick's view, from the energy manager. NAN = unknown. */
typedef struct {
    float   deye_soc, byd_soc;      /* %                                     */
    float   deye_bat_w;             /* Deye battery, + = discharging         */
    float   grid_w;                 /* + = import                            */
    float   pv_w, house_w;
    float   price_ct;               /* end price of the slot                 */
    uint8_t act;                    /* EM_ACT_* planned now, EH_ACT_NONE     */
    uint8_t mode;                   /* EM_MODE_*                             */
    bool    acting, paused;
} eh_sample_t;

/* A stored quarter hour. valid = false: nothing recorded for it. */
typedef struct {
    bool    valid;
    uint8_t act, mode, flags;
    float   deye_soc, byd_soc;      /* at the end of the slot, NAN unknown   */
    float   deye_bat_w, grid_w, pv_w, house_w;   /* means, NAN unknown       */
    float   price_ct;
} eh_slot_t;

/* Find or register the flash area and the newest record. */
esp_err_t em_hist_init(void);

/* Feed one sample at unix time `now`. When `now` lies in a new quarter hour
 * the previous one is written. */
void em_hist_tick(int64_t now, const eh_sample_t *s);

/* The quarter hours t0, t0 + 900, ... (n of them, t0 rounded down). Returns
 * how many were found. */
int em_hist_range(int64_t t0, int n, eh_slot_t *out);

/* Records stored, and the oldest slot's start (0 = none). */
int em_hist_count(int64_t *oldest);

#ifdef __cplusplus
}
#endif

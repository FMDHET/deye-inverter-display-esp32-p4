#pragma once

/* Efficiency sweep: drive the Deye through a list of fixed powers, 30 s each
 * by default, so the efficiency curve (mb_eff_curve_t) fills in a quarter of
 * an hour instead of weeks of normal operation -- which visits only the few
 * powers the house happens to need.
 *
 * Each step forces the battery to charge or discharge at one power through
 * deye_ctrl, waits `settle_s` for the inverter and its registers to settle,
 * then measures until `dwell_s`. The rounds that count for the curve go into
 * it on their own (modbus_rtu); the sweep keeps its own per-step means on top,
 * together with the grid meter. That second measurement is the cross-check:
 * between two steps the Deye's AC power moves by kilowatts while the house
 * load barely moves, so the grid meter (an independent, calibrated meter)
 * sees the same change. The slope between the two is the gain error of the
 * Deye's own AC register -- the page computes it from the step means.
 *
 * Safety, in that order:
 *   - the sweep never outranks the user: a mode or power set from anywhere
 *     else (display, MQTT, the 2-hour limit) ends it, and it then leaves the
 *     inverter alone instead of writing Normal over the user's choice;
 *   - SoC limits end a direction early (discharge above soc_min, charge
 *     below soc_max);
 *   - a Deye that stops answering for 30 s ends it, with Normal written;
 *   - a direction change goes through Normal for SWEEP_PAUSE_S: the charge
 *     set does not write the work mode, so going straight from Selling First
 *     to Forced-Charge would leave register 142 at Selling First;
 *   - one direction may not take longer than the 2-hour limit of a forced
 *     mode, because a retune does not restart that clock (deye_ctrl_retune);
 *   - the SLS export guard keeps working: it throttles a discharge step like
 *     any other, and the step simply measures the power it got.
 * Within a direction only the power is rewritten (one register when
 * discharging, two when charging), so a whole sweep costs a few dozen
 * EEPROM writes in the inverter. The results live in RAM; a restart ends a
 * sweep, and deye_ctrl undoes the forced mode it leaves behind. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "modbus_rtu.h"

#define SWEEP_MAX_POWERS   20
#define SWEEP_MAX_STEPS    (2 * SWEEP_MAX_POWERS)
#define SWEEP_PAUSE_S      60      /* Normal between the two directions      */
#define SWEEP_OFFLINE_S    30      /* Deye silent this long -> abort         */
#define SWEEP_DIR_MAX_S    (110 * 60)  /* under deye_ctrl's 2 h, with margin */

#define SWEEP_DIR_DIS      0x01
#define SWEEP_DIR_CHG      0x02

typedef enum {
    SWEEP_IDLE = 0,        /* never ran since boot                           */
    SWEEP_RUNNING,
    SWEEP_DONE,            /* every step done or skipped                     */
    SWEEP_ABORTED,         /* stopped, see `why`                             */
} eff_sweep_state_t;

typedef enum {
    STEP_PENDING = 0,
    STEP_SETTLE,           /* power applied, not measured yet                */
    STEP_MEASURE,
    STEP_DONE,
    STEP_SKIPPED,          /* SoC limit or abort before it started           */
} eff_sweep_step_state_t;

typedef struct {
    uint8_t  mode;         /* DEYE_MODE_FORCE_CHARGE / _FORCE_DISCHARGE       */
    uint8_t  state;        /* eff_sweep_step_state_t                         */
    uint16_t target_w;
    uint8_t  soc0, soc1;   /* at the start / end of the measuring window     */
    uint16_t n;            /* counted rounds in the measuring window         */
    uint16_t n_grid;       /* ... of those with a fresh grid reading         */
    /* Sums over the n counted rounds (W); the means are sum / n.
     * in/out as in the curve: discharge in = bat+PV, out = AC; charge
     * in = AC, out = bat+PV. ac is the Deye's AC power, + = delivers. */
    float    in_sum, out_sum, ac_sum, pv_sum;
    /* Over the n_grid rounds only: grid meter (+ = import) and the Deye's AC
     * power in exactly those rounds, so the two means are comparable. */
    float    grid_sum, ac_g_sum;
} eff_sweep_step_t;

typedef struct {
    uint8_t  dirs;                       /* SWEEP_DIR_*                       */
    uint8_t  npow;
    uint16_t pow[SWEEP_MAX_POWERS];      /* W, DEYE_POWER_FLOOR..MAX          */
    uint16_t dwell_s;                    /* per step, settling included, 30..1800 */
    uint16_t settle_s;
    uint8_t  soc_min, soc_max;           /* %                                 */
} eff_sweep_req_t;

typedef struct {
    uint8_t  state;                      /* eff_sweep_state_t                 */
    char     why[72];                    /* how it ended, German, for the page */
    uint8_t  nsteps, cur;
    bool     paused;                     /* in the Normal pause between dirs */
    eff_sweep_req_t req;
    uint32_t start_unix;                 /* 0 when the clock was not set     */
    uint32_t elapsed_s;
    uint32_t step_s;                     /* seconds into the current step    */
    uint32_t left_s;                     /* estimate until done              */
    uint32_t writes;                     /* inverter register writes so far  */
    eff_sweep_step_t step[SWEEP_MAX_STEPS];
} eff_sweep_status_t;

/* Start the task. Call once, after deye_ctrl_start(). */
void      eff_sweep_init(void);

/* Validate and start. On refusal returns an error and puts the reason
 * (German, for the page) into err. */
esp_err_t eff_sweep_start(const eff_sweep_req_t *rq, char *err, size_t errlen);

/* Stop a running sweep and put the inverter back to Normal. */
void      eff_sweep_stop(void);

void      eff_sweep_get(eff_sweep_status_t *out);
bool      eff_sweep_running(void);

#pragma once

/* SLS export guard -- the decision, without the I/O.
 *
 * During a forced discharge the Deye sells at the user's setpoint regardless
 * of what the house is doing, so the export can climb past what the house
 * connection fuse (SLS switch) is rated for. The guard lowers the inverter's
 * sell-power register until it fits.
 *
 * This lives apart from modbus_tcp.c on purpose. It steers an inverter that
 * hangs on the house installation, its interesting cases are "export has had
 * headroom for a minute" and "the correction is still not enough", and none
 * of that is something anyone wants to reproduce by standing in front of a
 * fuse box. As a pure function of (measurement, state, clock) it is testable
 * on the host; modbus_tcp.c keeps the reading of the meter, the mode and the
 * configuration, and calls in here for the answer.
 *
 * No floats leave this header's arithmetic beyond the measurement itself, no
 * allocation, no globals: the caller owns the state.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Granularity of a correction. Rounding every target to a step is also the
 * dead band: a change smaller than one step cannot produce a new target, so
 * meter noise on its own never reaches the inverter's EEPROM. */
#define SLS_STEP_W           500
/* Headroom the export must have before the guard gives power back ... */
#define SLS_HYST_W          1000
/* ... and how long it must keep that headroom. */
#define SLS_RESTORE_HOLD_S    60
/* Minimum gap between two guard writes. Registers 142/143 live in the
 * inverter's EEPROM, so every correction costs a write cycle. */
#define SLS_MIN_WRITE_MS   10000
/* An excess this large ignores that gap: the protective direction must never
 * wait on a rate limit. */
#define SLS_URGENT_W        2000

/* Caller-owned, zero-initialised. */
typedef struct {
    uint32_t below_ms;   /* when the export last started having headroom */
    uint32_t write_ms;   /* when the guard last wrote                    */
    bool     written;    /* ... whether it ever did                      */
} sls_guard_state_t;

typedef struct {
    bool  write;         /* true -> apply target_w                        */
    int   target_w;      /* new sell power (only meaningful when write)   */
    bool  urgent;        /* the rate limit was bypassed                   */
    float excess_w;      /* how far over the limit (0 when restoring)     */
} sls_decision_t;

/* Decide what to do with the sell power.
 *
 *   export_w      measured export (positive = feeding the grid)
 *   max_export_w  what the fuse allows
 *   applied_w     what the inverter is set to right now
 *   user_w        what the user asked for -- never exceeded, and the cap a
 *                 restore walks back up to
 *   now_ms        monotonic clock
 *   power_min_w   the inverter's lowest accepted sell power
 *
 * The correction is taken from applied_w, NOT from user_w. Measuring the error
 * against a setpoint the inverter is not currently following is what made the
 * original oscillate: it restored to user_w the moment the export fell under
 * the limit -- which it only had because of the throttle -- and it could even
 * raise the power while still over the limit (applied 9235 W, 500 W over,
 * "user_w - excess" = 9500 W). From applied_w the loop converges.
 *
 * Down is prompt, up is slow: the two directions are not equally urgent.
 * `st` is updated in place; a write is only reported when the caller should
 * actually perform it. */
static inline sls_decision_t sls_guard_decide(float export_w, float max_export_w,
                                              int applied_w, int user_w,
                                              uint32_t now_ms, int power_min_w,
                                              sls_guard_state_t *st)
{
    sls_decision_t d = { .write = false, .target_w = applied_w,
                         .urgent = false, .excess_w = 0 };
    int target = applied_w;

    if (export_w > max_export_w) {
        d.excess_w = export_w - max_export_w;
        target     = applied_w - (int)d.excess_w;
        d.urgent   = d.excess_w > SLS_URGENT_W;
        st->below_ms = 0;
    } else if (export_w < max_export_w - SLS_HYST_W && applied_w < user_w) {
        if (st->below_ms == 0) st->below_ms = now_ms;
        if ((uint32_t)(now_ms - st->below_ms) < (uint32_t)SLS_RESTORE_HOLD_S * 1000u)
            return d;
        target = applied_w + SLS_STEP_W;
    } else {
        st->below_ms = 0;    /* inside the hysteresis band: leave it alone */
        return d;
    }

    /* Round DOWN to a step -- the safe direction in both cases -- then clamp.
     * A second clamp used to sit in front of the rounding; a mutation test
     * showed it could not change any outcome. `%` truncates toward zero, so
     * rounding a target that is below the minimum lands at 500 W at most (or
     * at 0 from a negative one), and the clamp below catches every one of
     * them. The user's setpoint caps LAST, so a restore can land exactly on
     * it even when that value is not a multiple of the step. */
    target -= target % SLS_STEP_W;
    if (target < power_min_w) target = power_min_w;
    if (target > user_w)      target = user_w;
    if (target == applied_w)  return d;

    if (!d.urgent && st->written &&
        (uint32_t)(now_ms - st->write_ms) < SLS_MIN_WRITE_MS)
        return d;

    st->write_ms = now_ms;
    st->written  = true;
    st->below_ms = 0;                 /* a restore step restarts the hold */

    d.write    = true;
    d.target_w = target;
    return d;
}

#ifdef __cplusplus
}
#endif

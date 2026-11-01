#pragma once

/* The energy manager: one task that drives the feeds (prices, PV forecast,
 * load profile, Fronius storage), plans the Deye battery every quarter hour
 * (em_planner) and -- in EM_MODE_AUTO only -- carries the plan out:
 *
 *   CHARGE  deye_ctrl_apply_auto(): forced charge at the planned power, the
 *           target SoC written into the inverter, the window as long as the
 *           planned block (the inverter stops on its own at the target, and
 *           deye_ctrl ends the mode at the window's end even if we stall).
 *   HOLD    modbus_rtu_set_hold(): the meter emulation keeps the Deye from
 *           discharging; renewed every tick, expires on its own.
 *   NORMAL  whatever we set is released.
 *   BYD     while CHARGE or HOLD, and only if the user allowed it (byd_ctrl),
 *           the Fronius battery's discharge is blocked (self-reverting).
 *
 * The user always wins: a mode or power set from the display, MQTT or the
 * workbench pauses the automation until the end of the current block. A
 * running efficiency sweep pauses it too. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "em_types.h"
#include "em_planner.h"

typedef struct {
    uint8_t  mode;            /* EM_MODE_*                                    */
    int64_t  updated;         /* unix time of the last plan, 0 = none         */
    char     err[96];         /* why there is no plan, German, "" = fine      */
    uint8_t  act_now;         /* EM_ACT_* planned for the current slot        */
    bool     acting;          /* AUTO and actually applying it                */
    bool     paused;          /* the user took over                           */
    char     why[128];        /* the current decision in words, German        */
    float    price_now_ct;    /* NAN when unknown                             */
    float    ren_now_pct, solar_now_pct, wind_now_pct;
    float    pv_today_kwh, pv_tomorrow_kwh;
    float    saving_ct;       /* cost_base - cost_plan over the horizon       */
    int64_t  next_charge_from, next_charge_to;   /* next CHARGE block, 0 = none */
    float    deye_soc;
} em_status_t;

void em_mgr_start(void);                     /* after em_cfg_init, modbus_rtu, deye_ctrl */
void em_mgr_get_status(em_status_t *out);
void em_mgr_get_plan(em_plan_t *out);       /* ~5 kB: pass a static buffer   */
esp_err_t em_mgr_set_mode(uint8_t mode, char *err, size_t errlen);
/* "prices", "forecast", "plan" or "all": refetch / replan at the next tick. */
void em_mgr_request(const char *what);
/* The Fronius card is being looked at: keep reading the storage model for
 * the next two minutes even when nothing is controlled. */
void em_mgr_want_fronius(void);

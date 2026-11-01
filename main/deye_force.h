#pragma once

/* The operator's forced charge/discharge of the Deye battery from the web
 * page (or Home Assistant): power, a time, and a SoC at which it ends --
 * like the BYD's (fronius_storage_force).
 *
 * Runs through deye_ctrl_apply_auto(): the inverter itself falls back to
 * Normal when the time is up (deye_ctrl), and a forced CHARGE also has its
 * target SoC in the inverter's time-of-use registers. A forced DISCHARGE has
 * no such register: this module watches the SoC and ends it.
 *
 * Someone else changing the Deye in between (the display, the MQTT mode
 * select, the efficiency sweep) ends the session here without a write -- the
 * newer wish wins. Not kept over a restart: deye_ctrl undoes a forced mode it
 * finds in the inverter after a boot. The energy manager keeps out while a
 * forced mode is in force that is not its own. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DEYE_FORCE_OFF = 0, DEYE_FORCE_CHARGE = 1, DEYE_FORCE_DISCHARGE = 2 } deye_force_mode_t;

#define DEYE_FORCE_MIN_W    250
#define DEYE_FORCE_MAX_MIN  720          /* deye_ctrl's DEYE_AUTO_MAX_S */

typedef struct {
    uint8_t  mode;                       /* deye_force_mode_t, OFF = none   */
    uint16_t w;
    int64_t  until;                      /* unix                            */
    uint8_t  soc;                        /* charge: >=, discharge: <=       */
    bool     active;                     /* deye_ctrl runs it right now     */
    char     end[48];                    /* why the last one ended, German  */
} deye_force_t;

/* Start (w DEYE_FORCE_MIN_W..DEYE_POWER_MAX, minutes 1..DEYE_FORCE_MAX_MIN,
 * 0 = 60; soc 10..100) or end (OFF). ESP_ERR_INVALID_ARG out of range,
 * ESP_ERR_INVALID_STATE without a set clock, or the error of deye_ctrl. */
esp_err_t deye_force_set(deye_force_mode_t mode, uint16_t w, uint16_t minutes, uint8_t soc, int64_t now);

/* Every ~10 s from the energy manager task: SoC target, time, takeover. */
void      deye_force_tick(int64_t now);

void      deye_force_get(deye_force_t *out);

#ifdef __cplusplus
}
#endif

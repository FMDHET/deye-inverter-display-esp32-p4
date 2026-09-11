#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deye battery operating mode selectable from the dashboard popup. */
typedef enum {
    DEYE_MODE_NORMAL = 0,       /* inverter regulates normally               */
    DEYE_MODE_FORCE_CHARGE,     /* force the battery to charge   at power_w   */
    DEYE_MODE_FORCE_DISCHARGE,  /* force the battery to discharge at power_w   */
    DEYE_MODE_COUNT,
} deye_mode_t;

/* A forced mode is a standing intervention in the inverter's own control, so it
 * must not be able to outlive the display: after this long deye_ctrl falls back
 * to Normal on its own (and a restart resets it, see deye_ctrl_start). */
#define DEYE_FORCE_MAX_S  (2 * 60 * 60)

/* Power range deye_ctrl_apply() accepts (W). One definition for the LCD slider,
 * the HA discovery payload and the MQTT command parser -- the slider used to
 * offer 0..22000 while the backend silently clamped to this. */
#define DEYE_POWER_MIN  1000
#define DEYE_POWER_MAX  20000

/* Start the async writer task. Call once, AFTER modbus_rtu_start(). */
void        deye_ctrl_start(void);

/* Current mode + power. get_user_power() = what the user set via slider/MQTT;
 * get_power() may be lower when the SLS grid guard throttles the discharge. */
deye_mode_t deye_ctrl_get_mode(void);
int         deye_ctrl_get_power(void);
int         deye_ctrl_get_user_power(void);

const char *deye_ctrl_mode_name(deye_mode_t m);

/* What the last apply actually achieved, for the UI and the web page: a forced
 * mode whose register writes silently failed used to look identical to one that
 * worked. `failed` counts registers that did not come back with the value we
 * wrote (the decisive ones are read back, see deye_ctrl.c). */
typedef struct {
    deye_mode_t mode;
    int      power_w;        /* applied (may be throttled by the SLS guard) */
    int      user_power_w;   /* what the user asked for                     */
    uint32_t age_s;          /* since the mode was applied                  */
    uint32_t left_s;         /* until the automatic fallback (0 = Normal)   */
    uint8_t  checked;        /* registers read back on the last apply       */
    uint8_t  failed;         /* of those, how many did not match            */
    /* Register writes since boot. The Deye holds 142/143 in EEPROM, so this is
     * a wear counter, not a statistic: it is the number the SLS guard's rate
     * limit exists to keep small. Visible in /api/deye/live. */
    uint32_t writes;
} deye_ctrl_status_t;
void        deye_ctrl_get_status(deye_ctrl_status_t *out);

/* Apply a battery mode (user action: sets both user setpoint and applied power). */
esp_err_t deye_ctrl_apply(deye_mode_t mode, int power_w);

/* Throttle the discharge power without changing the user setpoint.
 * Called exclusively by the SLS grid-export guard in modbus_tcp.c.
 * Writes the sell-power register ALONE -- the work mode is already what it
 * should be, and register 142 lives in the inverter's EEPROM like 143 does.
 * A mode change requested in parallel still wins and writes both. */
esp_err_t deye_ctrl_set_throttled(int power_w);

#ifdef __cplusplus
}
#endif

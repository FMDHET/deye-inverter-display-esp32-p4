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

/* Start the async writer task. Call once, AFTER modbus_rtu_start(). */
void        deye_ctrl_start(void);

/* Current mode + power. get_user_power() = what the user set via slider/MQTT;
 * get_power() may be lower when the SLS grid guard throttles the discharge. */
deye_mode_t deye_ctrl_get_mode(void);
int         deye_ctrl_get_power(void);
int         deye_ctrl_get_user_power(void);

const char *deye_ctrl_mode_name(deye_mode_t m);

/* Apply a battery mode (user action: sets both user setpoint and applied power). */
esp_err_t deye_ctrl_apply(deye_mode_t mode, int power_w);

/* Throttle the discharge power without changing the user setpoint.
 * Called exclusively by the SLS grid-export guard in modbus_tcp.c. */
esp_err_t deye_ctrl_set_throttled(int power_w);

#ifdef __cplusplus
}
#endif

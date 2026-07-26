#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the GT911 over a new i2c-master bus on the board's I2C pins
 * and returns the touch handle. Coordinates are reported in panel-native
 * (portrait 480x800) space; landscape rotation is applied in lvgl_port.
 */
esp_err_t touch_init(esp_lcd_touch_handle_t *tp_out);

#ifdef __cplusplus
}
#endif

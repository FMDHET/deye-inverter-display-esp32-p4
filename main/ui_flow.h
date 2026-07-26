#pragma once

/* Energy-flow UI: five circular gauges (PV / House / Grid / Battery BYD /
 * Battery Deye) arranged in a pentagon around a central inverter icon.
 *
 * All setters expect the LVGL lock to be held. Power values are in kW;
 * positive = into the inverter (PV generation, grid import, battery
 * discharge), negative = away from inverter (export, charging).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_flow_create(void);

/* Update the build-number badge with the filesystem build read at boot.
 * fs_build < 0 means the asset FS was not mounted/readable. */
void ui_flow_set_fs_build(int fs_build);

/* Software contrast (grey-wash overlay on the top layer), percent 0..100. */
void ui_flow_set_contrast(uint8_t pct);

/* Display standby: blank the backlight after `seconds` of no touch (0 = off).
 * Touch wakes it. */
void ui_flow_set_sleep_timeout(uint32_t seconds);

void ui_flow_set_pv(float kw);             /* >= 0 */
void ui_flow_set_house(float kw);          /* >= 0 */
void ui_flow_set_grid(float kw_signed);    /* + import, - export */
void ui_flow_set_byd(float kw_signed, float soc_pct);
void ui_flow_set_deye(float kw_signed, float soc_pct);

/* Reset a node to the "--" placeholder when its source is gone (device disabled/
 * removed or value stale), so the dashboard never ghosts the last value. */
void ui_flow_clear_pv(void);
void ui_flow_clear_grid(void);
void ui_flow_clear_byd(void);
void ui_flow_clear_deye(void);
void ui_flow_clear_house(void);

#ifdef __cplusplus
}
#endif

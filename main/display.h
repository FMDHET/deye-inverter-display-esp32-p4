#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boots the MIPI-DSI PHY, brings up the ST7701 panel, and turns the
 * backlight on. After this returns, the framebuffer pipeline is live
 * and esp_lcd_panel_draw_bitmap() can be called on `panel`.
 */
esp_err_t display_init(esp_lcd_panel_handle_t *panel_out,
                       esp_lcd_panel_io_handle_t *io_out);

/* Backlight on (to the last set brightness) / off. Off is used to hide the
 * panel flicker while flashing. */
void display_backlight(bool on);

/* Set backlight brightness in percent via LEDC PWM. Values below
 * display_brightness_min() are raised to it: a user setting of 0 % was a black
 * panel that came back after every reboot. */
void display_set_brightness(uint8_t pct);

/* Lowest user brightness (percent). The settings slider starts here. */
uint8_t display_brightness_min(void);

#ifdef __cplusplus
}
#endif

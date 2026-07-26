#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up LVGL on top of the ST7701 MIPI-DSI panel and the GT911 touch
 * using Espressif's esp_lvgl_port (handles frame buffers, PPA-accelerated
 * 90 deg rotation and tear-free flushing). After this returns, lv_* calls
 * must be wrapped in app_lvgl_lock()/app_lvgl_unlock().
 */
esp_err_t app_lvgl_start(esp_lcd_panel_handle_t panel,
                         esp_lcd_panel_io_handle_t io,
                         esp_lcd_touch_handle_t tp);

/* Thin wrappers over esp_lvgl_port's lock. timeout_ms < 0 waits forever. */
bool app_lvgl_lock(int timeout_ms);
void app_lvgl_unlock(void);

/* Rotate the displayed image by `deg` (0/90/180/270) clockwise relative to the
 * normal landscape mounting. Persisted via nvs_store_*_orientation and applied
 * at boot from app_lvgl_start. Call from LVGL context (e.g. a UI callback) or
 * while holding app_lvgl_lock() -- it does NOT take the lock itself. */
void app_lvgl_apply_orientation(uint8_t deg);

#ifdef __cplusplus
}
#endif

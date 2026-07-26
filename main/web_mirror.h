#pragma once

/* Live web mirror of the LVGL display.
 *
 * Streams the actual on-screen image to a browser as MJPEG (the ESP32-P4
 * hardware JPEG encoder compresses each frame, read straight from the panel
 * framebuffer -- no re-render) and feeds pointer events from the browser back
 * into LVGL through a second input device -- so the web page shows and behaves
 * exactly like the physical touch display.
 *
 * Split over two HTTP servers to stay responsive: the page + /touch endpoint
 * live on the main :80 server (registered via web_mirror_register), while the
 * long-lived MJPEG stream runs on its own :81 server so it never blocks touch.
 */

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set up the JPEG encoder, web input device and the MJPEG stream server on :81.
 * `panel` is the live DPI panel whose framebuffer is read each frame. Call once,
 * after wifi_mgr_init() (needs lwIP up) and after LVGL is running. */
esp_err_t web_mirror_init(esp_lcd_panel_handle_t panel);

/* Register the mirror page ("/") and the "/touch" endpoint on an existing
 * HTTP server (the captive :80 server). */
void web_mirror_register(httpd_handle_t server);

/* Pause/resume the MJPEG snapshot loop. Pausing frees the LVGL lock and stops
 * re-rendering so the panel stays static (used during OTA to avoid flicker). */
void web_mirror_pause(bool paused);

#ifdef __cplusplus
}
#endif

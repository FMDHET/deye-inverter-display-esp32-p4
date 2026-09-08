#pragma once

/* Over-the-air firmware update via HTTP.
 *
 *   GET  /ota          -> JSON: version, build, fs_build, running/target slot,
 *                         uptime, reset reason, heap and DMA pool figures
 *   POST /ota          -> raw firmware.bin in the body; header is checked, then
 *                         written to the inactive OTA slot; device reboots into it
 *   POST /ota/fs       -> raw storage.bin (SPIFFS); remounted live, no reboot
 *   POST /ota/reboot   -> restart
 *   POST /ota/rollback -> boot the other slot and restart
 *   GET  /recovery     -> stand-alone rescue page (embedded, needs no assets)
 *
 * Flash over WiFi straight from the build output (the Expect: header avoids a
 * 1 s stall -- esp_http_server never answers 100-continue):
 *   curl -H 'Expect:' --data-binary @.pio/build/guition-p4/firmware.bin http://<ip>/ota
 *
 * NOTE: no authentication -- intended for a trusted local/dev network. Add a
 * token check before exposing the device more widely.
 */

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the GET/POST /ota routes on an existing HTTP server (:80). */
void ota_register_routes(httpd_handle_t server);

/* Confirm the running image so the bootloader keeps it (no-op unless it is
 * awaiting verification). Call once the device is remotely rescuable again --
 * see the comment on the implementation for why that is the criterion. */
void ota_mark_app_valid(void);

/* Start the probation timer for a freshly OTA'd image: confirms it via
 * ota_mark_app_valid() once the device has been up for a minute and is
 * reachable (STA has an IP or the SoftAP is up), or after ten minutes at the
 * latest. No-op for USB-flashed or already confirmed images. */
void ota_arm_confirm(void);

#ifdef __cplusplus
}
#endif

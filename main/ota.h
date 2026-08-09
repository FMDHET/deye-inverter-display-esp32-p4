#pragma once

/* Over-the-air firmware update via HTTP.
 *
 *   GET  /ota   -> JSON with the running version / partition (verify what runs)
 *   POST /ota   -> raw firmware.bin in the body; written to the inactive OTA
 *                  slot with esp_ota, then the device reboots into it.
 *
 * Flash over WiFi straight from the build output:
 *   curl --data-binary @.pio/build/guition-p4/firmware.bin http://<ip>/ota
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

#ifdef __cplusplus
}
#endif

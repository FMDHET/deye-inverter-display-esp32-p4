#pragma once

/* Backup and restore of the settings: GET /config, POST /config.
 *
 * Everything the owner ever typed lives in exactly one place -- the NVS
 * partition. WLAN passwords, the MQTT account, the device list and, worst of
 * all, the WireGuard PRIVATE KEY, which cannot be recovered from anywhere: a
 * damaged NVS page, an `nvs_flash_erase()` after a partition change, or a
 * simple mistake, and the tunnel has to be set up again from both ends.
 *
 * So: one JSON document out, the same document back in. It contains secrets by
 * design (a backup without the keys would be useless), which is why both
 * directions are behind the web password -- see webauth.h.
 */

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void config_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#pragma once

/* Password gate for everything that CHANGES something over HTTP.
 *
 * Until now anyone on the LAN could flash firmware (POST /ota), write any Deye
 * register (/deye/write) or drive the whole display through the mirror's
 * pointer injection (/touch). Reading stays open -- a dashboard nobody can look
 * at is useless, and the values are not secrets on a home network.
 *
 * The password is set ON THE DISPLAY (Einstellungen -> System), because
 * standing in front of the device is the one credential that cannot be faked
 * from the network. Empty password = gate off, exactly the old behaviour, so an
 * update changes nothing until the owner decides.
 *
 * HTTP Basic over plain HTTP: base64 is not encryption, so this keeps an
 * accident or a curious housemate out, not a determined attacker with a packet
 * sniffer on your LAN. TLS on an ESP32-P4 with a self-signed certificate would
 * trade that for a browser warning on every visit and a much bigger attack
 * surface -- not worth it here. Anything reachable from outside belongs behind
 * the WireGuard tunnel, which IS encrypted.
 */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_AUTH_PW_MAX 32

/* True when a password is set, i.e. the gate is active. */
bool      web_auth_enabled(void);

/* Gate a request. Returns true when the caller may proceed. On false the 401
 * (with WWW-Authenticate, so a browser asks) has already been sent -- just
 * `return ESP_OK`. */
bool      web_auth_ok(httpd_req_t *req);

/* Set/read the password. Empty string switches the gate off. */
esp_err_t web_auth_set(const char *pw);
esp_err_t web_auth_get(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

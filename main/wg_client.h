#pragma once

/* WireGuard VPN client. Brings up a WireGuard tunnel interface so the display
 * is reachable from a remote peer (e.g. for the web mirror / OTA over VPN).
 * Wraps the vendored esp_wireguard component. Config persists in NVS.
 *
 * Note: the WireGuard handshake needs a correct wall-clock time, so the tunnel
 * is only brought up after NTP has synced (see ntp_client). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persisted config (opaque NVS blob). Keys are base64 (wg genkey/pubkey). */
typedef struct {
    uint8_t  enabled;
    char     private_key[48];   /* this device's private key (base64)   */
    char     public_key[48];    /* peer/server public key (base64)      */
    char     preshared_key[48]; /* optional shared key (base64), "" off */
    char     address[20];       /* this device's tunnel IP, e.g. 10.6.0.2 */
    char     netmask[20];       /* tunnel netmask, e.g. 255.255.255.0   */
    char     endpoint[64];      /* peer host or IP                      */
    uint16_t port;              /* peer UDP port (default 51820)        */
    uint16_t keepalive;         /* persistent-keepalive seconds (0=off) */
} wg_cfg_t;

typedef struct {
    bool enabled;
    bool up;              /* peer handshake established */
    char address[20];     /* tunnel IP in use           */
} wg_status_t;

/* Start the WireGuard task (call once after WiFi is up). */
esp_err_t wg_start(void);

/* Settings-UI accessors (lazy-load from NVS on first use). */
void      wg_get_cfg(wg_cfg_t *out);
esp_err_t wg_set_cfg(const wg_cfg_t *cfg);

void      wg_get_status(wg_status_t *out);

#ifdef __cplusplus
}
#endif

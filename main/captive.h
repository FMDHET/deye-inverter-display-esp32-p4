#pragma once

/* Captive-portal WiFi provisioning.
 *
 * While the device runs its SoftAP (DeyeDisplay-XXXXXX), a phone or laptop
 * that joins it is automatically redirected to a setup page: a DNS hijack
 * resolves every hostname to the AP IP, and an HTTP server answers the OS
 * connectivity probes with a redirect so the "Sign in to network" sheet
 * pops up. The page scans for networks (esp_wifi via wifi_mgr) and submits
 * the chosen SSID + password, which wifi_mgr persists and connects to.
 *
 * This complements the on-device WLAN tab in ui_settings -- same backend
 * (wifi_mgr), different front-end.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the DNS hijack + HTTP server. Call once after wifi_mgr_init().
 * Idempotent; the servers idle until an AP client connects. */
esp_err_t captive_start(void);

/* Stop both servers and free resources. */
void captive_stop(void);

#ifdef __cplusplus
}
#endif

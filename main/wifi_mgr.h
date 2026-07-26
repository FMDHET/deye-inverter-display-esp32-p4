#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_BOOTING = 0,
    WIFI_MGR_STA_CONNECTING,
    WIFI_MGR_STA_CONNECTED,
    WIFI_MGR_AP_FALLBACK,   /* AP up; STA may still be auto-retrying */
    WIFI_MGR_AP_ONLY,       /* no STA creds saved -> AP only          */
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char     sta_ssid[33];
    char     sta_ip[16];        /* "xxx.xxx.xxx.xxx" or "" */
    char     sta_gw[16];        /* gateway when connected, else "" */
    int8_t   rssi;              /* 0 if not connected */
    char     ap_ssid[33];
    char     ap_ip[16];         /* "192.168.4.1" when AP up, else "" */
    bool     ap_active;
    char     mac[18];           /* STA MAC "AA:BB:CC:DD:EE:FF" */
} wifi_mgr_status_t;

typedef struct {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;          /* wifi_auth_mode_t */
} wifi_mgr_ap_t;

/* A saved STA credential. Up to WIFI_MGR_MAX_CREDS are remembered; the device
 * connects to whichever one is reachable. */
#define WIFI_MGR_MAX_CREDS 10
typedef struct {
    char ssid[33];
    char psk[65];
} wifi_cred_t;

/* Bring up ESP-Hosted + esp_wifi. After this returns, the manager
 * will try STA from saved credentials, and start AP fallback after a
 * timeout if STA fails. */
esp_err_t wifi_mgr_init(void);

void wifi_mgr_get_status(wifi_mgr_status_t *out);

/* Non-blocking scan. Call wifi_mgr_scan_results once the scan-done
 * event has fired (or simply poll wifi_mgr_scan_busy()). */
esp_err_t wifi_mgr_scan_start(void);
bool      wifi_mgr_scan_busy(void);
size_t    wifi_mgr_scan_results(wifi_mgr_ap_t *out, size_t max);

/* Persist new STA credentials (added/updated in the saved list) and connect to
 * them immediately. Used by the captive portal and the "connect" UI. */
esp_err_t wifi_mgr_set_sta(const char *ssid, const char *psk);

/* Forget ALL saved STA credentials and bring the device into AP_ONLY mode. */
esp_err_t wifi_mgr_clear_sta(void);

/* ---- Saved-credential list management (up to WIFI_MGR_MAX_CREDS) ---- */
/* Copy the saved credentials into out[]; returns the count. */
int       wifi_mgr_get_creds(wifi_cred_t *out, int max);
/* Add a new network or update the PSK of an existing one (by SSID). Persists.
 * Returns ESP_ERR_NO_MEM if the list is full and the SSID is new. */
esp_err_t wifi_mgr_add_cred(const char *ssid, const char *psk);
/* Remove a saved network by SSID. Persists. If it was the active one, the
 * manager re-selects another saved network (or drops to AP). */
esp_err_t wifi_mgr_remove_cred(const char *ssid);
/* Connect now to an already-saved network (by SSID), using its stored PSK. */
esp_err_t wifi_mgr_connect_saved(const char *ssid);
/* SSID currently connected/targeted (for UI highlighting); "" if none. */
void      wifi_mgr_get_active_ssid(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

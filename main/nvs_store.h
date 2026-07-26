#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialises NVS (call once from app_main). Handles full-flash by
 * erasing the partition and re-initialising. */
esp_err_t nvs_store_init(void);

/* ------------- WiFi STA credentials ------------- */
/* On success, ssid/psk are filled and ESP_OK is returned.
 * Returns ESP_ERR_NOT_FOUND if no credentials are saved.        */
esp_err_t nvs_store_get_sta(char *ssid, size_t ssid_sz,
                            char *psk,  size_t psk_sz);
esp_err_t nvs_store_set_sta(const char *ssid, const char *psk);
esp_err_t nvs_store_clear_sta(void);
bool      nvs_store_has_sta(void);

/* Multiple STA credentials (SSID+PSK list), stored as an opaque blob.
 * *len is in/out for get. clear erases the whole list. */
esp_err_t nvs_store_get_wifi_list(void *buf, size_t *len);
esp_err_t nvs_store_set_wifi_list(const void *buf, size_t len);
esp_err_t nvs_store_clear_wifi_list(void);

/* ------------- WiFi AP password (optional override) -------- */
/* AP SSID is always "DeyeDisplay-XXYYZZ" (MAC suffix); only the
 * passphrase is configurable. Default falls back to "deyedisplay". */
esp_err_t nvs_store_get_ap_psk(char *psk, size_t psk_sz);
esp_err_t nvs_store_set_ap_psk(const char *psk);

/* ------------- Display settings (percent 0..100) -------------
 * Brightness drives the LEDC backlight PWM; contrast a software overlay.
 * Defaults: brightness 80, contrast 100 (= full, no overlay).            */
uint8_t   nvs_store_get_brightness(void);
esp_err_t nvs_store_set_brightness(uint8_t pct);
uint8_t   nvs_store_get_contrast(void);
esp_err_t nvs_store_set_contrast(uint8_t pct);

/* Display standby: turn the backlight off after this many seconds of no
 * touch (0 = never). */
uint16_t  nvs_store_get_sleep_secs(void);
esp_err_t nvs_store_set_sleep_secs(uint16_t secs);

/* Display orientation: how far the panel image is rotated clockwise from the
 * normal landscape mounting. One of 0 / 90 / 180 / 270 (default 0). 90 and 270
 * are portrait. */
uint8_t   nvs_store_get_orientation(void);
esp_err_t nvs_store_set_orientation(uint8_t deg);

/* ------------- Modbus-TCP (Deye inverter) ------------- */
esp_err_t nvs_store_get_mb_ip(char *buf, size_t sz);  /* "" if unset */
esp_err_t nvs_store_set_mb_ip(const char *ip);
uint16_t  nvs_store_get_mb_port(void);                /* default 502  */
esp_err_t nvs_store_set_mb_port(uint16_t port);
uint8_t   nvs_store_get_mb_unit(void);                /* default 1    */
esp_err_t nvs_store_set_mb_unit(uint8_t unit);
uint16_t  nvs_store_get_mb_interval(void);            /* ms, def 2000 */
esp_err_t nvs_store_set_mb_interval(uint16_t ms);
bool      nvs_store_get_mb_enabled(void);             /* default off  */
esp_err_t nvs_store_set_mb_enabled(bool enabled);

/* Modbus device list, stored/loaded as an opaque blob (array of structs). */
esp_err_t nvs_store_get_mb_devices(void *buf, size_t *len);  /* *len in/out */
esp_err_t nvs_store_set_mb_devices(const void *buf, size_t len);
/* Reads the previous-layout ("devs4", 42B) blob, for one-time migration. */
esp_err_t nvs_store_get_mb_devices_legacy(void *buf, size_t *len);

/* Modbus-RTU config blob (mb_rtu_cfg_t). */
esp_err_t nvs_store_get_mb_rtu(void *buf, size_t len);
esp_err_t nvs_store_set_mb_rtu(const void *buf, size_t len);

/* MQTT forwarding config blob (mqtt_cfg_t). */
esp_err_t nvs_store_get_mqtt(void *buf, size_t len);
esp_err_t nvs_store_set_mqtt(const void *buf, size_t len);

/* NTP / clock config blob (ntp_cfg_t). */
esp_err_t nvs_store_get_ntp(void *buf, size_t len);
esp_err_t nvs_store_set_ntp(const void *buf, size_t len);

/* WireGuard VPN config blob (wg_cfg_t). */
esp_err_t nvs_store_get_wg(void *buf, size_t len);
esp_err_t nvs_store_set_wg(const void *buf, size_t len);

/* Grid setpoint (W) for the Eastron-emulation zero-export trick. Default 0. */
int       nvs_store_get_grid_sp(void);
esp_err_t nvs_store_set_grid_sp(int w);

/* SLS (main fuse) rating in ampere. 0 = guard disabled. Default 35. */
uint8_t   nvs_store_get_sls_a(void);
esp_err_t nvs_store_set_sls_a(uint8_t a);

#ifdef __cplusplus
}
#endif

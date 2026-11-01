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

/* Password for the WRITING web endpoints (OTA, Deye register writes, the
 * mirror's pointer injection, config export). Empty = no password, which is the
 * behaviour every device had before this existed. Set on the display, see
 * webauth.h for why. */
esp_err_t nvs_store_get_web_pw(char *pw, size_t pw_sz);
esp_err_t nvs_store_set_web_pw(const char *pw);

/* ------------- Display settings (percent 0..100) -------------
 * Brightness drives the LEDC backlight PWM; contrast a software overlay.
 * Defaults: brightness 80, contrast 100 (= full, no overlay).            */
uint8_t   nvs_store_get_brightness(void);
esp_err_t nvs_store_set_brightness(uint8_t pct);
uint8_t   nvs_store_get_contrast(void);
esp_err_t nvs_store_set_contrast(uint8_t pct);

/* Standard section order of the /deye workbench ("live,curve,..."), for every
 * browser that has not sorted on its own. Only [a-z0-9,], shorter than
 * NVS_SEC_ORDER_MAX; anything else is refused (ESP_ERR_INVALID_ARG). An empty
 * string erases it, a missing or invalid record reads as "". */
#define NVS_SEC_ORDER_MAX 128
bool      nvs_store_sec_order_ok(const char *s);
esp_err_t nvs_store_get_sec_order(char *buf, size_t len);
esp_err_t nvs_store_set_sec_order(const char *s);

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
/* Reads a previous-layout device blob ("devs5" 66 B, "devs4" 42 B records) for
 * the one-time migration. The old keys are left in place, so a rollback to an
 * older firmware still finds its device list. */
esp_err_t nvs_store_get_mb_devices_legacy(const char *key, void *buf, size_t *len);

/* Modbus-RTU config blob (mb_rtu_cfg_t). */
esp_err_t nvs_store_get_mb_rtu(void *buf, size_t len);
esp_err_t nvs_store_set_mb_rtu(const void *buf, size_t len);

/* BYD charged/discharged energy integrated on the display (byd_energy.c). */
esp_err_t nvs_store_get_byd_energy(void *buf, size_t len);
esp_err_t nvs_store_set_byd_energy(const void *buf, size_t len);

/* Long-term Deye conversion energy (mb_deye_eff_lt_t). */
esp_err_t nvs_store_get_deye_eff(void *buf, size_t len);
esp_err_t nvs_store_set_deye_eff(const void *buf, size_t len);
/* Efficiency by power (mb_eff_curve_t). The write is refused with
 * ESP_ERR_NVS_NOT_ENOUGH_SPACE while the partition is nearly full, so the
 * statistic can never crowd out a setting. */
esp_err_t nvs_store_get_deye_effc(void *buf, size_t len);
esp_err_t nvs_store_set_deye_effc(const void *buf, size_t len);
/* NVS entries (32 bytes each) still available for data, 0 if unknown. */
size_t    nvs_store_free_entries(void);
/* Energy manager settings (em_cfg_t, see em_cfg.h). Configuration: part of
 * the settings backup. */
esp_err_t nvs_store_get_em_cfg(void *buf, size_t len);
esp_err_t nvs_store_set_em_cfg(const void *buf, size_t len);
/* Energy manager state (not in the settings backup): PV forecast
 * calibration and the household load profile. The load profile write is
 * refused while NVS is nearly full, like the efficiency curve. */
esp_err_t nvs_store_get_pv_cal(void *buf, size_t len);
esp_err_t nvs_store_set_pv_cal(const void *buf, size_t len);
esp_err_t nvs_store_get_load_prof(void *buf, size_t len);
/* Fronius storage block state (fronius_storage.c): whether a discharge
 * block of ours is in the inverter, and the values to restore. */
esp_err_t nvs_store_get_fs_state(void *buf, size_t len);
esp_err_t nvs_store_set_fs_state(const void *buf, size_t len);
/* The operator's lock of the Deye battery (deye_lock.c), "em/dlock". */
esp_err_t nvs_store_get_dlock(void *buf, size_t len);
esp_err_t nvs_store_set_dlock(const void *buf, size_t len);
esp_err_t nvs_store_set_load_prof(const void *buf, size_t len);
/* Energy counters of the Fronius producer meter (fronius_meter.c). State, not
 * configuration: not part of the settings backup. */
esp_err_t nvs_store_get_frm_energy(void *buf, size_t len);
esp_err_t nvs_store_set_frm_energy(const void *buf, size_t len);

/* Per-phase manipulation blob (mb_manip_cfg_t). Absent -> manipulation off. */
esp_err_t nvs_store_get_mb_manip(void *buf, size_t len);
esp_err_t nvs_store_set_mb_manip(const void *buf, size_t len);

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

/* Forced battery mode (deye_mode_t) and its power, persisted so a reboot can
 * UNDO it: the inverter keeps a forced mode across our restart, the display
 * used to come back saying "Normal" while the Deye kept charging. Not restored
 * on boot -- deye_ctrl writes Normal to the inverter instead. */
uint8_t   nvs_store_get_deye_mode(void);
esp_err_t nvs_store_set_deye_mode(uint8_t mode);
uint16_t  nvs_store_get_deye_power(void);
esp_err_t nvs_store_set_deye_power(uint16_t w);

#ifdef __cplusplus
}
#endif

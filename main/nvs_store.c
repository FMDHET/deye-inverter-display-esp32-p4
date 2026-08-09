#include "nvs_store.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_store";

#define NS_WIFI "wifi"
#define K_STA_SSID "sta_ssid"
#define K_STA_PSK  "sta_psk"
#define K_AP_PSK   "ap_psk"

esp_err_t nvs_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition damaged, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t get_str(const char *ns, const char *key,
                         char *buf, size_t bufsz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = bufsz;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err;
}

static esp_err_t set_str(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_key(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_store_get_sta(char *ssid, size_t ssid_sz,
                            char *psk,  size_t psk_sz)
{
    esp_err_t err = get_str(NS_WIFI, K_STA_SSID, ssid, ssid_sz);
    if (err != ESP_OK) return err;
    err = get_str(NS_WIFI, K_STA_PSK, psk, psk_sz);
    /* Empty PSK (open network) is valid; treat NOT_FOUND as "". */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        psk[0] = '\0';
        err = ESP_OK;
    }
    return err;
}

esp_err_t nvs_store_set_sta(const char *ssid, const char *psk)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    esp_err_t err = set_str(NS_WIFI, K_STA_SSID, ssid);
    if (err != ESP_OK) return err;
    return set_str(NS_WIFI, K_STA_PSK, psk ? psk : "");
}

esp_err_t nvs_store_clear_sta(void)
{
    erase_key(NS_WIFI, K_STA_SSID);
    erase_key(NS_WIFI, K_STA_PSK);
    return ESP_OK;
}

bool nvs_store_has_sta(void)
{
    char ssid[33];
    return get_str(NS_WIFI, K_STA_SSID, ssid, sizeof(ssid)) == ESP_OK
           && ssid[0] != '\0';
}

/* ---- Multiple STA credentials (opaque blob: array of wifi_cred_t) ---- */
#define K_STA_LIST "stalist"

esp_err_t nvs_store_get_wifi_list(void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_WIFI, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, K_STA_LIST, buf, len);
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_set_wifi_list(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_WIFI, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, K_STA_LIST, buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_clear_wifi_list(void)
{
    return erase_key(NS_WIFI, K_STA_LIST);
}

esp_err_t nvs_store_get_ap_psk(char *psk, size_t psk_sz)
{
    esp_err_t err = get_str(NS_WIFI, K_AP_PSK, psk, psk_sz);
    if (err != ESP_OK) {
        strncpy(psk, "deyedisplay", psk_sz - 1);
        psk[psk_sz - 1] = '\0';
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t nvs_store_set_ap_psk(const char *psk)
{
    if (!psk || strlen(psk) < 8) return ESP_ERR_INVALID_ARG;
    return set_str(NS_WIFI, K_AP_PSK, psk);
}

/* ------------- Display settings ------------- */
#define NS_DISP    "disp"
#define K_BRIGHT   "bright"
#define K_CONTRAST "contrast"
#define K_ORIENT   "orient"

static uint8_t get_u8(const char *ns, const char *key, uint8_t defval)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return defval;
    uint8_t v = defval;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = defval;
    nvs_close(h);
    return v;
}

static esp_err_t set_u8(const char *ns, const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint8_t   nvs_store_get_brightness(void)        { return get_u8(NS_DISP, K_BRIGHT, 80); }
esp_err_t nvs_store_set_brightness(uint8_t pct) { return set_u8(NS_DISP, K_BRIGHT, pct); }
uint8_t   nvs_store_get_contrast(void)          { return get_u8(NS_DISP, K_CONTRAST, 100); }
esp_err_t nvs_store_set_contrast(uint8_t pct)   { return set_u8(NS_DISP, K_CONTRAST, pct); }
uint8_t   nvs_store_get_orientation(void)       { return get_u8(NS_DISP, K_ORIENT, 0); }
esp_err_t nvs_store_set_orientation(uint8_t deg){ return set_u8(NS_DISP, K_ORIENT, deg); }

#define K_SLEEP "sleep"

static uint16_t get_u16(const char *ns, const char *key, uint16_t defval)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return defval;
    uint16_t v = defval;
    if (nvs_get_u16(h, key, &v) != ESP_OK) v = defval;
    nvs_close(h);
    return v;
}

static esp_err_t set_u16(const char *ns, const char *key, uint16_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint16_t  nvs_store_get_sleep_secs(void)         { return get_u16(NS_DISP, K_SLEEP, 0); }
esp_err_t nvs_store_set_sleep_secs(uint16_t s)   { return set_u16(NS_DISP, K_SLEEP, s); }

/* ------------- Modbus-TCP (Deye) ------------- */
#define NS_MB "modbus"

esp_err_t nvs_store_get_mb_ip(char *buf, size_t sz)
{
    esp_err_t e = get_str(NS_MB, "ip", buf, sz);
    if (e != ESP_OK && sz) buf[0] = '\0';
    return e;
}
esp_err_t nvs_store_set_mb_ip(const char *ip)     { return set_str(NS_MB, "ip", ip ? ip : ""); }
uint16_t  nvs_store_get_mb_port(void)             { return get_u16(NS_MB, "port", 502); }
esp_err_t nvs_store_set_mb_port(uint16_t p)       { return set_u16(NS_MB, "port", p); }
uint8_t   nvs_store_get_mb_unit(void)             { return get_u8(NS_MB, "unit", 1); }
esp_err_t nvs_store_set_mb_unit(uint8_t u)        { return set_u8(NS_MB, "unit", u); }
uint16_t  nvs_store_get_mb_interval(void)         { return get_u16(NS_MB, "intv", 2000); }
esp_err_t nvs_store_set_mb_interval(uint16_t ms)  { return set_u16(NS_MB, "intv", ms); }
bool      nvs_store_get_mb_enabled(void)          { return get_u8(NS_MB, "en", 0) != 0; }
esp_err_t nvs_store_set_mb_enabled(bool en)       { return set_u8(NS_MB, "en", en ? 1 : 0); }

esp_err_t nvs_store_get_mb_devices(void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, "devs5", buf, len);   /* devs5: +name */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_get_mb_devices_legacy(void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, "devs4", buf, len);   /* prev layout (42B, +timeout_ms) */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_set_mb_devices(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "devs5", buf, len);   /* devs5: +name */
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_get_mb_rtu(void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t l = len;
    e = nvs_get_blob(h, "rtu2", buf, &l);
    nvs_close(h);
    /* A SHORTER record is an older layout: mb_rtu_cfg_t only ever grows at the
     * end (the bus[] array keeps its stride), and the caller pre-zeroes, so the
     * appended fields read 0 and clamp_cfg() gives them their defaults. Only a
     * LONGER record -- a downgrade to firmware that predates those fields --
     * is rejected, since we cannot know what it holds. */
    if (e == ESP_OK && l > len) e = ESP_ERR_INVALID_SIZE;
    return e;
}

esp_err_t nvs_store_set_mb_rtu(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "rtu2", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

#define NS_MQTT "mqtt"

esp_err_t nvs_store_get_mqtt(void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MQTT, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t l = len;
    e = nvs_get_blob(h, "cfg", buf, &l);   /* buffer is pre-zeroed by caller; a
                                              shorter (older-layout) blob is kept */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_set_mqtt(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MQTT, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "cfg", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

#define NS_NTP "ntp"

esp_err_t nvs_store_get_ntp(void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_NTP, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t l = len;
    e = nvs_get_blob(h, "cfg", buf, &l);   /* buffer pre-zeroed by caller; a
                                              shorter (older-layout) blob is kept */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_set_ntp(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_NTP, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "cfg", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

#define NS_WG "wg"

esp_err_t nvs_store_get_wg(void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_WG, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t l = len;
    e = nvs_get_blob(h, "cfg", buf, &l);   /* buffer pre-zeroed by caller */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_set_wg(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_WG, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "cfg", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

int nvs_store_get_grid_sp(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_MB, NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t v = 0;
    nvs_get_i32(h, "gsp", &v);
    nvs_close(h);
    return (int)v;
}

esp_err_t nvs_store_set_grid_sp(int w)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i32(h, "gsp", (int32_t)w);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

uint8_t nvs_store_get_sls_a(void)        { return get_u8(NS_MB, "sls_a", 35); }
esp_err_t nvs_store_set_sls_a(uint8_t a) { return set_u8(NS_MB, "sls_a", a); }

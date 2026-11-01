#include "nvs_store.h"

#include <stdlib.h>
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

/* Read a fixed-layout config blob, and cope with BOTH directions of a size
 * mismatch.
 *
 * These structs only ever grow at the end. A SHORTER stored record is an older
 * layout: nvs_get_blob fills what there is, the caller pre-zeroes, and the
 * appended fields read 0 -> the owning module gives them its defaults.
 *
 * A LONGER record is the rollback case, and it used to lose everything:
 * nvs_get_blob does not truncate, it returns ESP_ERR_NVS_INVALID_LENGTH when
 * the buffer is too small, so the older firmware saw "no config" and silently
 * fell back to defaults -- MQTT account, clock, VPN keys, gone from its point
 * of view, and overwritten as soon as anything saved. Since fields are only
 * appended, the first `len` bytes of a newer record ARE the older struct, so
 * read the whole thing and keep the prefix.
 *
 * This is why there is no version field: it would have to be read by exactly
 * the firmware that cannot read the record in the first place. The prefix rule
 * needs no cooperation from the future -- only that fields keep being appended,
 * which is already the rule for every one of these structs. */
static esp_err_t get_blob_prefix(const char *ns, const char *key,
                                 void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    size_t stored = 0;
    e = nvs_get_blob(h, key, NULL, &stored);      /* how big is it really? */
    if (e != ESP_OK) { nvs_close(h); return e; }

    if (stored <= len) {
        size_t l = len;
        e = nvs_get_blob(h, key, buf, &l);
        nvs_close(h);
        return e;
    }

    /* Newer, longer record: read it whole, keep the part we understand. */
    void *tmp = malloc(stored);
    if (!tmp) { nvs_close(h); return ESP_ERR_NO_MEM; }
    size_t l = stored;
    e = nvs_get_blob(h, key, tmp, &l);
    nvs_close(h);
    if (e == ESP_OK) {
        memcpy(buf, tmp, len);
        ESP_LOGW(TAG, "%s/%s: %u bytes stored, this firmware knows %u -- "
                      "keeping the part it understands (rollback?)",
                 ns, key, (unsigned)stored, (unsigned)len);
    }
    free(tmp);
    return e;
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

esp_err_t nvs_store_get_web_pw(char *pw, size_t pw_sz)
{
    if (!pw || pw_sz == 0) return ESP_ERR_INVALID_ARG;
    if (get_str(NS_WIFI, "web_pw", pw, pw_sz) != ESP_OK) pw[0] = '\0';
    return ESP_OK;                       /* "nothing stored" = no password */
}

esp_err_t nvs_store_set_web_pw(const char *pw)
{
    return set_str(NS_WIFI, "web_pw", pw ? pw : "");
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

/* Section order of the /deye workbench: the page's own section names,
 * comma-separated. Only that alphabet goes in -- it ends up in a JSON string
 * and in the page, so nothing that needs escaping can ever be stored. */
#define K_SEC_ORDER "sec_order"

bool nvs_store_sec_order_ok(const char *s)
{
    if (!s) return false;
    size_t n = strlen(s);
    if (n >= NVS_SEC_ORDER_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == ','))
            return false;
    return true;
}

esp_err_t nvs_store_get_sec_order(char *buf, size_t len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    esp_err_t e = get_str(NS_DISP, K_SEC_ORDER, buf, len);
    if (e != ESP_OK || !nvs_store_sec_order_ok(buf)) buf[0] = '\0';
    return e;
}

esp_err_t nvs_store_set_sec_order(const char *s)
{
    if (!nvs_store_sec_order_ok(s)) return ESP_ERR_INVALID_ARG;
    if (!s[0]) {                                   /* empty = back to built-in */
        esp_err_t e = erase_key(NS_DISP, K_SEC_ORDER);
        return e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
    }
    return set_str(NS_DISP, K_SEC_ORDER, s);
}

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
    e = nvs_get_blob(h, "devs6", buf, len);   /* devs6: +invert */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_get_mb_devices_legacy(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, key, buf, len);       /* devs5 (66B, +name), devs4 (42B) */
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_set_mb_devices(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "devs6", buf, len);   /* devs6: +invert */
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_get_mb_rtu(void *buf, size_t len)
{
    return get_blob_prefix(NS_MB, "rtu2", buf, len);
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

esp_err_t nvs_store_get_byd_energy(void *buf, size_t len)
{
    return get_blob_prefix(NS_MB, "byd_e", buf, len);
}

esp_err_t nvs_store_set_byd_energy(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "byd_e", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t nvs_store_get_deye_eff(void *buf, size_t len)
{
    return get_blob_prefix(NS_MB, "deye_eff", buf, len);
}

esp_err_t nvs_store_set_deye_eff(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "deye_eff", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* The efficiency curve is ~0.5 kB of state in a 24 kB partition that also
 * holds WiFi, MQTT and VPN credentials. NVS writes a changed blob next to the
 * old one before dropping it, so a rewrite needs its own size in free entries
 * on top of what it occupies. Below EFFC_RESERVE free entries the curve is not
 * written at all: losing a quarter hour of statistics is cheap, a setting
 * that cannot be saved is not. */
#define EFFC_RESERVE_ENTRIES 128   /* 4 kB headroom beyond the reserved page */

esp_err_t nvs_store_get_deye_effc(void *buf, size_t len)
{
    return get_blob_prefix(NS_MB, "deye_effc", buf, len);
}

esp_err_t nvs_store_set_deye_effc(const void *buf, size_t len)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK &&
        st.available_entries < EFFC_RESERVE_ENTRIES + (len + 31) / 32)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "deye_effc", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

size_t nvs_store_free_entries(void)
{
    nvs_stats_t st;
    return nvs_get_stats(NULL, &st) == ESP_OK ? st.available_entries : 0;
}

/* Energy manager settings (em_cfg_t): location, PV planes, capacities, the
 * Tibber token. Its own namespace, so it can be wiped on its own. */
#define NS_EM "em"

esp_err_t nvs_store_get_em_cfg(void *buf, size_t len)
{
    return get_blob_prefix(NS_EM, "cfg", buf, len);
}

esp_err_t nvs_store_set_em_cfg(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_EM, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "cfg", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* State of the energy manager's learning parts, same namespace: the PV
 * forecast's per-plane calibration ("pvcal") and the household load profile
 * ("load"). State, not configuration: not part of the settings backup. */
static esp_err_t em_blob_set(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_EM, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, key, buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
esp_err_t nvs_store_get_pv_cal(void *buf, size_t len)        { return get_blob_prefix(NS_EM, "pvcal", buf, len); }
esp_err_t nvs_store_set_pv_cal(const void *buf, size_t len)  { return em_blob_set("pvcal", buf, len); }
esp_err_t nvs_store_get_load_prof(void *buf, size_t len)     { return get_blob_prefix(NS_EM, "load", buf, len); }
esp_err_t nvs_store_get_fs_state(void *buf, size_t len)      { return get_blob_prefix(NS_EM, "fsblk", buf, len); }
esp_err_t nvs_store_set_fs_state(const void *buf, size_t len){ return em_blob_set("fsblk", buf, len); }
esp_err_t nvs_store_get_dlock(void *buf, size_t len)         { return get_blob_prefix(NS_EM, "dlock", buf, len); }
esp_err_t nvs_store_set_dlock(const void *buf, size_t len)   { return em_blob_set("dlock", buf, len); }
esp_err_t nvs_store_set_load_prof(const void *buf, size_t len)
{
    /* Same rule as the efficiency curve: statistics never crowd out settings. */
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK &&
        st.available_entries < EFFC_RESERVE_ENTRIES + (len + 31) / 32)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    return em_blob_set("load", buf, len);
}

esp_err_t nvs_store_get_frm_energy(void *buf, size_t len)
{
    return get_blob_prefix(NS_MB, "frm_e", buf, len);
}

esp_err_t nvs_store_set_frm_energy(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "frm_e", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* Per-phase manipulation of the values served to the Deye (mb_manip_cfg_t).
 * Its own key so it can be wiped without touching the bus config, and so an
 * older firmware simply ignores it (= manipulation off). */
esp_err_t nvs_store_get_mb_manip(void *buf, size_t len)
{
    return get_blob_prefix(NS_MB, "manip1", buf, len);
}

esp_err_t nvs_store_set_mb_manip(const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_MB, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "manip1", buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

#define NS_MQTT "mqtt"

esp_err_t nvs_store_get_mqtt(void *buf, size_t len)
{
    return get_blob_prefix(NS_MQTT, "cfg", buf, len);
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
    return get_blob_prefix(NS_NTP, "cfg", buf, len);
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
    return get_blob_prefix(NS_WG, "cfg", buf, len);
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

uint8_t   nvs_store_get_deye_mode(void)        { return get_u8(NS_MB, "dmode", 0); }
esp_err_t nvs_store_set_deye_mode(uint8_t m)   { return set_u8(NS_MB, "dmode", m); }
uint16_t  nvs_store_get_deye_power(void)       { return get_u16(NS_MB, "dpwr", 0); }
esp_err_t nvs_store_set_deye_power(uint16_t w) { return set_u16(NS_MB, "dpwr", w); }

#include "em_cfg.h"
#include "nvs_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "em_cfg";

/* The layout is the NVS record and part of the settings backup: pin it. */
_Static_assert(sizeof(em_plane_t) == 28, "em_plane_t layout");
_Static_assert(sizeof(em_cfg_t) == 12 + EM_MAX_PLANES * 28 + 12 + EM_TOKEN_MAX + 16, "em_cfg_t layout");

static em_cfg_t     s_cfg = { .ver = EM_CFG_VER, .charge_kw = EM_DEF_CHARGE_KW,
                              .min_spread_ct = EM_DEF_MIN_SPREAD, .cycle_cost_ct = EM_DEF_CYCLE_COST,
                              .soc_min_pct = EM_DEF_SOC_MIN, .soc_max_pct = EM_DEF_SOC_MAX };

/* A record from before a field existed has it at 0: fill in the default. */
static void defaults(em_cfg_t *c)
{
    if (c->charge_kw == 0.0f)     c->charge_kw     = EM_DEF_CHARGE_KW;
    if (c->min_spread_ct == 0.0f) c->min_spread_ct = EM_DEF_MIN_SPREAD;
    if (c->soc_max_pct == 0)      c->soc_max_pct   = EM_DEF_SOC_MAX;
}
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t refuse(char *err, size_t len, const char *why)
{
    if (err && len) snprintf(err, len, "%s", why);
    return ESP_ERR_INVALID_ARG;
}

static bool in_range(float v, float lo, float hi) { return isfinite(v) && v >= lo && v <= hi; }

/* Check and tidy a candidate. Pure: the host test drives it directly.
 * Names: printable ASCII and UTF-8 only (they go into JSON with escaping, but
 * a control character has no business in "Süd"), always terminated. */
static esp_err_t check(em_cfg_t *c, char *err, size_t len)
{
    if (c->nplanes > EM_MAX_PLANES) return refuse(err, len, "höchstens 6 PV-Felder");
    if (!(c->lat == 0.0f && c->lon == 0.0f) &&
        (!in_range(c->lat, -90.0f, 90.0f) || !in_range(c->lon, -180.0f, 180.0f)))
        return refuse(err, len, "Standort: Breite -90…90, Länge -180…180");
    for (int i = 0; i < c->nplanes; i++) {
        em_plane_t *p = &c->plane[i];
        p->name[sizeof(p->name) - 1] = '\0';
        for (char *s = p->name; *s; s++)
            if ((unsigned char)*s < 0x20 || *s == '"' || *s == '\\') *s = ' ';
        if (!in_range(p->kwp, 0.1f, 100.0f))
            return refuse(err, len, "PV-Feld: Leistung 0,1 bis 100 kWp");
        if (p->tilt < 0 || p->tilt > 90)
            return refuse(err, len, "PV-Feld: Neigung 0 bis 90°");
        if (p->azimuth < 0 || p->azimuth > 359)
            return refuse(err, len, "PV-Feld: Ausrichtung 0 bis 359° (0 = Nord, 90 = Ost, 180 = Süd, 270 = West)");
        if (!(p->meter == EM_METER_NONE || p->meter == EM_METER_DEYE ||
              (p->meter >= 0 && p->meter < 8)))
            return refuse(err, len, "PV-Feld: unbekannte Messung");
        if (p->ac_w > EM_PLANE_AC_MAX_W)
            return refuse(err, len, "PV-Feld: AC-Grenze 0 (keine) bis 60 kW");
        p->_rsv = 0;
    }
    for (int i = c->nplanes; i < EM_MAX_PLANES; i++) memset(&c->plane[i], 0, sizeof(c->plane[i]));
    if (!in_range(c->deye_kwh, 0.0f, 500.0f) || !in_range(c->byd_kwh, 0.0f, 500.0f))
        return refuse(err, len, "Speicher: 0 bis 500 kWh");
    if (c->byd_reserve_pct > 100) return refuse(err, len, "BYD-Reserve 0 bis 100 %");
    if (c->byd_max_soc >= 100) c->byd_max_soc = 0;               /* 100 % = no limit */
    if (c->byd_max_soc && c->byd_max_soc < 50)
        return refuse(err, len, "Obere Ladegrenze BYD 50 bis 100 % (100 = aus)");
    if (!in_range(c->charge_kw, 0.25f, 20.0f)) return refuse(err, len, "Ladeleistung 0,25 bis 20 kW");
    if (!in_range(c->min_spread_ct, 0.0f, 100.0f) || !in_range(c->cycle_cost_ct, 0.0f, 100.0f))
        return refuse(err, len, "Mindestvorteil und Verschleiß 0 bis 100 ct/kWh");
    if (c->soc_max_pct > 100 || c->soc_min_pct + 10 > c->soc_max_pct)
        return refuse(err, len, "SoC-Grenzen: max. bis 100 %, mindestens 10 % über min.");
    if (c->mode > EM_MODE_AUTO) return refuse(err, len, "unbekannte Betriebsart");
    c->byd_ctrl = c->byd_ctrl ? 1 : 0;
    c->ver = EM_CFG_VER;
    c->_rsv0 = 0;
    memset(c->_rsv1, 0, sizeof(c->_rsv1));
    return ESP_OK;
}

/* Tibber tokens are base64url-ish. Anything else is a paste accident. */
static bool token_ok(const char *t)
{
    size_t n = strlen(t);
    if (n >= EM_TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = t[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.'))
            return false;
    }
    return true;
}

void em_cfg_init(void)
{
    em_cfg_t c;
    memset(&c, 0, sizeof(c));
    if (nvs_store_get_em_cfg(&c, sizeof(c)) != ESP_OK) return;
    defaults(&c);
    char err[80];
    c.tibber_token[EM_TOKEN_MAX - 1] = '\0';
    if (check(&c, err, sizeof(err)) != ESP_OK || !token_ok(c.tibber_token)) {
        ESP_LOGW(TAG, "stored settings invalid (%s), ignored", err);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_cfg = c;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%u PV planes, Deye %.1f kWh, BYD %.1f kWh, Tibber %s",
             c.nplanes, c.deye_kwh, c.byd_kwh, c.tibber_token[0] ? "set" : "not set");
}

void em_cfg_get(em_cfg_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_mux);
}

bool em_cfg_has_token(void)
{
    portENTER_CRITICAL(&s_mux);
    bool b = s_cfg.tibber_token[0] != '\0';
    portEXIT_CRITICAL(&s_mux);
    return b;
}

static esp_err_t store(const em_cfg_t *c, char *err, size_t len)
{
    if (nvs_store_set_em_cfg(c, sizeof(*c)) != ESP_OK)
        return refuse(err, len, "Speichern im NVS fehlgeschlagen");
    portENTER_CRITICAL(&s_mux);
    s_cfg = *c;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t em_cfg_set(const em_cfg_t *in, char *err, size_t len)
{
    if (!in) return refuse(err, len, "keine Angaben");
    em_cfg_t c = *in;
    defaults(&c);
    esp_err_t e = check(&c, err, len);
    if (e != ESP_OK) return e;
    portENTER_CRITICAL(&s_mux);
    memcpy(c.tibber_token, s_cfg.tibber_token, sizeof(c.tibber_token));   /* kept */
    portEXIT_CRITICAL(&s_mux);
    e = store(&c, err, len);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "settings saved: %u PV planes, %.4f/%.4f", c.nplanes, c.lat, c.lon);
    return e;
}

esp_err_t em_cfg_set_token(const char *tok, char *err, size_t len)
{
    if (!tok || !token_ok(tok)) return refuse(err, len, "Tibber-Token: nur Buchstaben, Ziffern, - _ . (höchstens 95 Zeichen)");
    em_cfg_t c;
    em_cfg_get(&c);
    memset(c.tibber_token, 0, sizeof(c.tibber_token));
    memcpy(c.tibber_token, tok, strlen(tok));
    esp_err_t e = store(&c, err, len);
    if (e == ESP_OK) ESP_LOGI(TAG, "Tibber token %s", tok[0] ? "stored" : "deleted");
    return e;
}

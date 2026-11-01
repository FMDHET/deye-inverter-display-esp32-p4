#include "pv_forecast.h"
#include "keep_ram.h"
#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR: the big series live in PSRAM */
#include "https_get.h"
#include "modbus_rtu.h"      /* the Deye's own PV strings, for calibration  */
#include "modbus_tcp.h"      /* PV meters / inverters on Modbus-TCP          */
#include "nvs_store.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#ifndef MALLOC_CAP_SPIRAM    /* the host test brings its own two-line shim */
#include "esp_heap_caps.h"
#endif
#include "freertos/FreeRTOS.h"

static const char *TAG = "pv_fc";

/* The answer is ~10 kB (192 quarter hours x time + five values, 49 hourly
 * snow depths). 20 kB leaves room for a longer DST day and an error text, and
 * lives in PSRAM: it is only needed for the seconds of a fetch. */
#define PV_BUF_CAP      (20 * 1024)
#define PV_URL_CAP      512
/* A gap between two ticks longer than this is not integrated: the task was
 * blocked (a fetch, a stalled bus) and the power at either end says nothing
 * about the time in between. */
#define PV_MAX_GAP_S    120
#define PV_NIGHT_KW     0.001   /* group forecast below this: night */
/* 2: the operator's physical model replaced Open-Meteo's tilted irradiance
 * (2026-10-22) -- factors learnt against the old model are dropped.
 * 3: the Symo Hybrid's battery port was counted as PV (modbus_tcp, fixed
 * 2026-10-24) -- the "Süd" factor had learnt from battery discharge. */
#define PV_CAL_VER      3

/* Backoff after a failed fetch: 5, 10, 20, then 60 minutes. Open-Meteo is free
 * and rate-limited; hammering it after a 429 only prolongs the block. */
static const int32_t BACKOFF_S[] = { 300, 600, 1200, 3600 };
#define BACKOFF_N  ((int)(sizeof(BACKOFF_S) / sizeof(BACKOFF_S[0])))

/* The persisted factors. `sig` identifies the plane geometry the factor was
 * learnt for: a factor learnt for 5 kWp south is meaningless for 8 kWp east,
 * so a changed plane starts over at 1.0. Fields are only ever appended. */
typedef struct {
    uint16_t ver;                 /* PV_CAL_VER                                */
    uint8_t  n;                   /* planes stored                             */
    uint8_t  _rsv;
    struct {
        uint32_t sig;             /* plane_sig() of kwp/tilt/azimuth/meter     */
        float    cal;
    } p[EM_MAX_PLANES];
} pv_cal_rec_t;

/* ---- published state, guarded by s_mux ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR pv_fc_t      s_pub;

/* ---- owned by the tick (one task) ---- */
static bool          s_inited;
static em_cfg_t      s_cfg;
static EXT_RAM_BSS_ATTR em_series_t   s_raw[EM_MAX_PLANES];     /* uncalibrated kW, as fetched     */
static uint32_t      s_raw_sig[EM_MAX_PLANES]; /* geometry s_raw was fetched for  */
static EXT_RAM_BSS_ATTR em_series_t   s_tmp;                    /* parse target, kept off the stack */
static float         s_cal[EM_MAX_PLANES];
static uint32_t      s_sig[EM_MAX_PLANES];     /* geometry s_cal belongs to       */
static uint8_t       s_nsig;
static int64_t       s_fetched;
static int64_t       s_next_fetch;
static uint8_t       s_backoff;
static volatile bool s_req;
static char          s_err[80];
static bool          s_err_pre;                /* s_err is a missing precondition */

/* Today's integration. Group sums are kept at the index of the group's first
 * plane (its "leader"): planes on the same meter share one measurement, so
 * only the sum of their forecasts can be compared with it. */
static int64_t       s_day_t0;                   /* local day being integrated  */
static int64_t       s_last_tick;
static double        s_grp_meas_kwh[EM_MAX_PLANES]; /* measured, while forecast known */
static double        s_grp_fc_kwh[EM_MAX_PLANES];   /* uncalibrated forecast, same time */
static double        s_meas_kwh[EM_MAX_PLANES];     /* per plane, for display          */

/* Today's measurement per quarter hour and plane. Kept in memory that is
 * not cleared at boot (keep_ram.h): a software restart or an OTA (several a day while
 * developing) must not leave the day's curve with a hole from midnight to
 * the restart. A power cut does clear it -- the magic, the checksum and the
 * date tell a valid copy from leftover noise or another firmware's layout.
 * Identified per plane by its meter: a changed geometry keeps the
 * measurement (the panels made the same energy), a changed meter does not. */
#define PV_KEEP_MAGIC 0x50564D32u   /* "PVM2": since the battery port fix, see PV_CAL_VER 3 */
typedef struct {
    uint32_t magic;
    uint32_t size;                                   /* sizeof, a layout check */
    int64_t  day_t0;
    int8_t   meter[EM_MAX_PLANES];
    uint8_t  _rsv[2];
    float    kwh[EM_MAX_PLANES][PV_MEAS_SLOTS];
    uint16_t sec[EM_MAX_PLANES][PV_MEAS_SLOTS];      /* seconds covered, <= 900 */
    uint32_t crc;                                    /* FNV-1a over all above   */
} pv_keep_t;
/* In the LP RAM object of keep_ram.h (a stable address from build to build). */
_Static_assert(sizeof(pv_keep_t) <= sizeof(g_keep_ram.pv), "pv_keep_t fits its keep area");
#define s_keep (*(pv_keep_t *)(void *)g_keep_ram.pv)

static mb_dev_live_t  s_dev[MB_MAX_DEVICES];
static mb_deye_live_t s_deye;

/* ------------------------------ time ----------------------------------- */

static int64_t local_midnight(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;                 /* let mktime find out: DST days exist */
    return (int64_t)mktime(&tm);
}

/* Start of the local day after the one starting at day_t0. Not +86400: the
 * two DST days have 23 and 25 hours. */
static int64_t next_midnight(int64_t day_t0)
{
    time_t tt = (time_t)day_t0;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_mday += 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant). timegm()
 * is not in newlib, and mktime() would apply the local zone. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* "2026-10-21T00:15" (UTC, we ask for timezone=UTC) -> unix seconds. */
static bool parse_utc(const char *s, int64_t *out)
{
    int y, mo, d, h, mi;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) != 5) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    *out = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60;
    return true;
}

static void fmt_utc(int64_t t, char out[20])
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, 20, "%Y-%m-%dT%H:%M", &tm);
}

/* ------------------------------ model ---------------------------------- */

/* The operator's PV model (their Home Assistant template sensors, adopted
 * 2026-10-22), per plane and quarter hour from the location's weather:
 *   sun position  Spencer (declination, equation of time)
 *   transposition HDKR (Hay-Davies-Klucher-Reindl) from GHI, DHI, DNI
 *   reflection    Martin-Ruiz IAM (a_r 0.16) on the beam part
 *   cell temp     Faiman, T_cell = T_air + POA / (25 + 6.84 wind)
 *   module        Huld (PVGIS c-Si coefficients)
 *   losses        8 % (PV_FC_PR), snow derate below 60 deg tilt
 * The AC limit is applied by the caller (raw_at, publish), after the
 * calibration factor. Weather values are quarter-hour means, so the sun is
 * taken at the middle of the slot. */
typedef struct pv_wx {
    float ghi, dhi, dni;     /* W/m^2, NAN unknown                        */
    float tair, wind;        /* C, m/s                                     */
    float snow_cm;           /* snow depth                                 */
} pv_wx_t;

#define DEG2RAD(x) ((x) * (float)M_PI / 180.0f)

/* Day of year (1..366) and hours since midnight, both UTC. */
static void utc_doy(int64_t t, int *doy, float *hr)
{
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    *doy = tm.tm_yday + 1;
    *hr  = (float)tm.tm_hour + tm.tm_min / 60.0f + tm.tm_sec / 3600.0f;
}

/* cos(zenith) and sun azimuth (compass degrees) at t. */
static void sun_pos(int64_t t, float lat, float lon, float *cosz, float *az)
{
    int   doy;
    float hr;
    utc_doy(t, &doy, &hr);
    float B   = 2.0f * (float)M_PI * ((float)(doy - 1) + (hr - 12.0f) / 24.0f) / 365.0f;
    float dec = 0.006918f - 0.399912f * cosf(B) + 0.070257f * sinf(B) - 0.006758f * cosf(2 * B)
              + 0.000907f * sinf(2 * B) - 0.002697f * cosf(3 * B) + 0.00148f * sinf(3 * B);
    float eot = 229.18f * (0.000075f + 0.001868f * cosf(B) - 0.032077f * sinf(B)
                           - 0.014615f * cosf(2 * B) - 0.040849f * sinf(2 * B));
    float hra = (hr * 60.0f + eot + 4.0f * lon) / 4.0f - 180.0f;
    float la  = DEG2RAD(lat);
    float cz  = sinf(la) * sinf(dec) + cosf(la) * cosf(dec) * cosf(DEG2RAD(hra));
    cz = fmaxf(-1.0f, fminf(1.0f, cz));
    float sz = sqrtf(1.0f - cz * cz);
    float a  = 180.0f;
    if (sz > 1e-6f) {
        float c = (sinf(dec) - sinf(la) * cz) / (cosf(la) * sz);
        a = acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 180.0f / (float)M_PI;
        if (hra > 0.0f) a = 360.0f - a;           /* afternoon: mirrored */
    }
    *cosz = cz;
    *az   = a;
}

/* Huld relative efficiency, PVGIS coefficients for crystalline silicon. */
static float huld_eta(float g, float tcell)
{
    static const float k1 = -0.017237f, k2 = -0.040465f, k3 = -0.004702f,
                       k4 = 0.000149f,  k5 = 0.00017f,   k6 = 0.000005f;
    float lg = logf(g), tp = tcell - 25.0f;
    return 1.0f + k1 * lg + k2 * lg * lg + tp * (k3 + k4 * lg + k5 * lg * lg) + k6 * tp * tp;
}

/* Plane-of-array irradiance, W/m^2. */
static float pv_poa(const em_plane_t *p, const pv_wx_t *w, int64_t t, float lat, float lon)
{
    float cz, saz;
    sun_pos(t, lat, lon, &cz, &saz);
    int   doy;
    float hr;
    utc_doy(t, &doy, &hr);
    float tb   = DEG2RAD((float)p->tilt);
    float sz   = sqrtf(fmaxf(0.0f, 1.0f - cz * cz));
    float caoi = cz * cosf(tb) + sz * sinf(tb) * cosf(DEG2RAD(saz - (float)p->azimuth));
    /* Sun behind the plane or below the horizon: no beam, no circumsolar. */
    float cp   = (cz > 0.0f && caoi > 0.0f) ? caoi : 0.0f;
    float dni  = isfinite(w->dni) ? fmaxf(0.0f, w->dni) : 0.0f;
    float dhi  = fmaxf(0.0f, w->dhi);
    float e0   = 1367.0f * (1.0f + 0.033f * cosf(2.0f * (float)M_PI * (float)doy / 365.0f));
    float ai   = fminf(1.0f, dni / e0);
    float rb   = cp / fmaxf(cz, 0.087f);              /* capped near 85 deg zenith */
    float f    = sqrtf(fmaxf(0.0f, (w->ghi - dhi) / w->ghi));
    float s3   = sinf(tb / 2.0f);
    float iso  = (1.0f + cosf(tb)) / 2.0f * (1.0f + f * s3 * s3 * s3);
    float alb  = (isfinite(w->snow_cm) && w->snow_cm > 1.0f) ? 0.65f : 0.2f;
    float gnd  = w->ghi * alb * (1.0f - cosf(tb)) / 2.0f;
    float iam  = cp > 0.0f ? (1.0f - expf(-cp / 0.16f)) / (1.0f - expf(-1.0f / 0.16f)) : 0.0f;
    return dni * cp * iam + dhi * (ai * rb + (1.0f - ai) * iso) + gnd;
}

/* AC kW of one plane before calibration and AC limit. */
static float pv_plane_kw(const em_plane_t *p, const pv_wx_t *w, int64_t t, float lat, float lon)
{
    if (!isfinite(w->ghi) || !isfinite(w->dhi)) return NAN;
    if (w->ghi <= 0.0f) return 0.0f;
    float poa = pv_poa(p, w, t, lat, lon);
    if (!(poa > 0.0f)) return 0.0f;
    float wind  = isfinite(w->wind) ? fmaxf(0.0f, w->wind) : 1.0f;
    float tair  = isfinite(w->tair) ? w->tair : 15.0f;
    float tcell = tair + poa / (25.0f + 6.84f * wind);
    float g     = poa / 1000.0f;
    float kw    = p->kwp * g * fmaxf(0.0f, huld_eta(g, tcell)) * PV_FC_PR;
    if (p->tilt < 60 && isfinite(w->snow_cm)) {
        float s = w->snow_cm;
        kw *= s > 5.0f ? 0.05f : s > 2.0f ? 0.3f : s > 0.5f ? 0.7f : 1.0f;
    }
    return kw;
}

/* The host suite puts a simpler plane model here to test everything around
 * it (calibration, measurement, schedule); the physics has its own tests. */
#ifndef PV_FORECAST_PLANE_KW
#define PV_FORECAST_PLANE_KW pv_plane_kw
#endif

/* Weather for the two local days, slot k = [s_wx_t0 + 900 k, +900). */
static EXT_RAM_BSS_ATTR pv_wx_t s_wx[EM_MAX_SLOTS];
static EXT_RAM_BSS_ATTR pv_wx_t s_wx_tmp[EM_MAX_SLOTS];
static int64_t s_wx_t0;
static int     s_wx_n;                    /* 0 = no weather yet */

/* Walk two cJSON arrays side by side: cJSON_GetArrayItem would be quadratic. */
static float jnum_next(const cJSON **c)
{
    float v = NAN;
    if (*c) {
        if (cJSON_IsNumber(*c)) v = (float)(*c)->valuedouble;
        *c = (*c)->next;
    }
    return v;
}

/* Parse an Open-Meteo answer into s_wx_tmp (t0, n slots). Returns the number
 * of time stamps that landed in a slot, -1 when the answer is not usable.
 *
 * Open-Meteo's 15-minutely radiation is the mean over the PRECEDING quarter
 * hour: the value stamped 12:15 describes 12:00..12:15. Our slot i covers
 * [t0 + 900 i, t0 + 900 (i+1)), so a value stamped T goes into the slot that
 * starts at T - 900. Taken unshifted, the forecast would run 15 min late --
 * small for a sum, but the planner cares in which quarter hour the sun comes.
 * Snow depth is hourly (metres): each slot takes the hour it starts in. */
static int parse_wx(const char *buf, size_t len, int64_t t0, int n)
{
    for (int k = 0; k < EM_MAX_SLOTS; k++)
        s_wx_tmp[k] = (pv_wx_t){ NAN, NAN, NAN, NAN, NAN, 0.0f };

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) return -1;
    const cJSON *m  = cJSON_GetObjectItemCaseSensitive(root, "minutely_15");
    const cJSON *tt = cJSON_GetObjectItemCaseSensitive(m, "time");
    const cJSON *gh = cJSON_GetObjectItemCaseSensitive(m, "shortwave_radiation");
    const cJSON *dh = cJSON_GetObjectItemCaseSensitive(m, "diffuse_radiation");
    if (!cJSON_IsArray(tt) || !cJSON_IsArray(gh) || !cJSON_IsArray(dh)) {
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *dn = cJSON_GetObjectItemCaseSensitive(m, "direct_normal_irradiance");
    const cJSON *tp = cJSON_GetObjectItemCaseSensitive(m, "temperature_2m");
    const cJSON *ws = cJSON_GetObjectItemCaseSensitive(m, "wind_speed_10m");
    const cJSON *a = tt->child, *g = gh->child, *d = dh->child;
    const cJSON *b = cJSON_IsArray(dn) ? dn->child : NULL;
    const cJSON *c = cJSON_IsArray(tp) ? tp->child : NULL;
    const cJSON *v = cJSON_IsArray(ws) ? ws->child : NULL;
    int mapped = 0;
    for (; a; a = a->next) {
        pv_wx_t w;
        w.ghi = jnum_next(&g); w.dhi = jnum_next(&d); w.dni = jnum_next(&b);
        w.tair = jnum_next(&c); w.wind = jnum_next(&v); w.snow_cm = 0.0f;
        int64_t ts;
        if (!cJSON_IsString(a) || !parse_utc(a->valuestring, &ts)) continue;
        int64_t slot = ts - EM_SLOT_S;
        if (slot < t0 || (slot - t0) % EM_SLOT_S != 0) continue;
        int64_t k = (slot - t0) / EM_SLOT_S;
        if (k >= n) continue;
        s_wx_tmp[k] = w;
        mapped++;
    }
    const cJSON *h  = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(h, "time");
    const cJSON *sd = cJSON_GetObjectItemCaseSensitive(h, "snow_depth");
    if (cJSON_IsArray(ht) && cJSON_IsArray(sd)) {
        const cJSON *s = sd->child;
        for (const cJSON *x = ht->child; x; x = x->next) {
            float depth = jnum_next(&s);
            int64_t hs;
            if (!cJSON_IsString(x) || !parse_utc(x->valuestring, &hs) || !isfinite(depth)) continue;
            for (int k = 0; k < n; k++) {
                int64_t st = t0 + (int64_t)k * EM_SLOT_S;
                if (st >= hs && st < hs + 3600) s_wx_tmp[k].snow_cm = depth * 100.0f;
            }
        }
    }
    cJSON_Delete(root);
    return mapped;
}

/* Plane i's uncalibrated series from the stored weather. A changed plane
 * needs no new fetch: the weather is the same for every orientation. */
static void compute_raw(int i)
{
    em_series_t *o = &s_raw[i];
    o->t0 = s_wx_t0;
    o->n  = (uint16_t)s_wx_n;
    for (int k = 0; k < EM_MAX_SLOTS; k++)
        o->v[k] = k < s_wx_n
                ? PV_FORECAST_PLANE_KW(&s_cfg.plane[i], &s_wx[k],
                                       s_wx_t0 + (int64_t)k * EM_SLOT_S + EM_SLOT_S / 2,
                                       s_cfg.lat, s_cfg.lon)
                : NAN;
    s_raw_sig[i] = s_sig[i];
}

/* ---------------------------- calibration ------------------------------ */

/* FNV-1a over the fields that make a factor meaningless when they change.
 * The name is left out on purpose: renaming "Süd" to "Dach" keeps it. */
static uint32_t plane_sig(const em_plane_t *p)
{
    uint8_t b[4 + 2 + 2 + 1];
    memcpy(b, &p->kwp, 4);
    memcpy(b + 4, &p->tilt, 2);
    memcpy(b + 6, &p->azimuth, 2);
    memcpy(b + 8, &p->meter, 1);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(b); i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;        /* 0 means "no plane" */
}

static float clamp_cal(float c)
{
    if (!isfinite(c)) return 1.0f;
    return c < PV_CAL_MIN ? PV_CAL_MIN : (c > PV_CAL_MAX ? PV_CAL_MAX : c);
}

static void save_cal(void)
{
    static pv_cal_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.ver = PV_CAL_VER;
    rec.n   = s_nsig;
    for (int i = 0; i < s_nsig; i++) { rec.p[i].sig = s_sig[i]; rec.p[i].cal = s_cal[i]; }
    esp_err_t e = nvs_store_set_pv_cal(&rec, sizeof(rec));
    if (e != ESP_OK) ESP_LOGW(TAG, "Kalibrierung nicht gespeichert: %s", esp_err_to_name(e));
}

static void load_cal(void)
{
    static pv_cal_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    for (int i = 0; i < EM_MAX_PLANES; i++) { s_cal[i] = 1.0f; s_sig[i] = 0; }
    s_nsig = 0;
    if (nvs_store_get_pv_cal(&rec, sizeof(rec)) != ESP_OK) return;
    if (rec.ver != PV_CAL_VER || rec.n > EM_MAX_PLANES) {
        ESP_LOGW(TAG, "Kalibrierdatensatz v%u/%u verworfen", rec.ver, rec.n);
        return;
    }
    s_nsig = rec.n;
    for (int i = 0; i < rec.n; i++) { s_sig[i] = rec.p[i].sig; s_cal[i] = clamp_cal(rec.p[i].cal); }
}

static uint32_t keep_crc(void)
{
    const uint8_t *b = (const uint8_t *)&s_keep;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(pv_keep_t, crc); i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static void keep_seal(void) { s_keep.crc = keep_crc(); }

static void keep_clear_plane(int j)
{
    memset(s_keep.kwh[j], 0, sizeof(s_keep.kwh[j]));
    memset(s_keep.sec[j], 0, sizeof(s_keep.sec[j]));
    s_keep.meter[j] = j < s_cfg.nplanes ? s_cfg.plane[j].meter : EM_METER_NONE;
}

/* A new day (or the first tick after boot): take over what a software
 * restart left of today, else start empty. */
static void keep_open_day(int64_t day_t0)
{
    bool valid = s_keep.magic == PV_KEEP_MAGIC && s_keep.size == sizeof(pv_keep_t) &&
                 s_keep.crc == keep_crc() && s_keep.day_t0 == day_t0;
    if (!valid) {
        memset(&s_keep, 0, sizeof(s_keep));
        s_keep.magic  = PV_KEEP_MAGIC;
        s_keep.size   = sizeof(pv_keep_t);
        s_keep.day_t0 = day_t0;
        for (int j = 0; j < EM_MAX_PLANES; j++) keep_clear_plane(j);
    } else {
        int kept = 0;
        for (int j = 0; j < EM_MAX_PLANES; j++) {
            int8_t m = j < s_cfg.nplanes ? s_cfg.plane[j].meter : EM_METER_NONE;
            if (s_keep.meter[j] != m) { keep_clear_plane(j); continue; }
            double e = 0.0;
            for (int k = 0; k < PV_MEAS_SLOTS; k++) e += s_keep.kwh[j][k];
            s_meas_kwh[j] = e;
            if (e > 0.0) kept++;
        }
        ESP_LOGI(TAG, "Messung von heute über den Neustart gerettet (%d Felder)", kept);
    }
    keep_seal();
}

static void reset_day_acc(void)
{
    memset(s_grp_meas_kwh, 0, sizeof(s_grp_meas_kwh));
    memset(s_grp_fc_kwh, 0, sizeof(s_grp_fc_kwh));
    memset(s_meas_kwh, 0, sizeof(s_meas_kwh));
}

/* Take over the planes of the settings. A plane whose geometry or meter
 * differs from what its factor was learnt for starts over at 1.0. Any change
 * also drops today's sums (the groups may have changed shape) and asks for a
 * new forecast -- the old one was computed for the old plane. */
static void sync_planes(void)
{
    bool changed = s_nsig != s_cfg.nplanes;
    for (int i = 0; i < s_cfg.nplanes; i++) {
        uint32_t sig = plane_sig(&s_cfg.plane[i]);
        if (i < s_nsig && sig == s_sig[i]) continue;
        if (i < s_nsig) ESP_LOGI(TAG, "Feld %d geändert: Kalibrierung zurück auf 1,0", i);
        s_sig[i] = sig;
        s_cal[i] = 1.0f;
        changed  = true;
    }
    if (!changed) return;
    for (int i = s_cfg.nplanes; i < EM_MAX_PLANES; i++) { s_sig[i] = 0; s_cal[i] = 1.0f; }
    s_nsig = s_cfg.nplanes;
    save_cal();
    reset_day_acc();
    /* No refetch: the stored weather serves any orientation (tick recomputes). */
}

static bool raw_ok(int i) { return s_raw_sig[i] != 0 && s_raw_sig[i] == s_sig[i]; }

/* AC limit of plane i (kW), +inf for none. A string of 8.5 kWp behind a 5 kW
 * inverter never delivers more than 5 kW, however bright the sky. */
static float ac_kw(int i)
{
    return s_cfg.plane[i].ac_w ? s_cfg.plane[i].ac_w / 1000.0f : INFINITY;
}

/* Uncalibrated forecast of plane i at t, kW, already capped at the AC limit:
 * the meter that calibrates it is capped too, so clipped hours compare like
 * with like instead of teaching the factor that the plane "underperforms". */
static float raw_at(int i, int64_t t)
{
    if (!raw_ok(i)) return NAN;
    float v = em_series_at(&s_raw[i], t);
    return isfinite(v) ? fminf(v, ac_kw(i)) : v;
}

/* The first plane on the same meter: where the group's sums live. */
static int group_leader(int i)
{
    for (int j = 0; j < i; j++)
        if (s_cfg.plane[j].meter == s_cfg.plane[i].meter) return j;
    return i;
}

/* Measured PV power of a meter in W; false when there is no fresh value. */
static bool meter_w(int8_t meter, int ndev, float *w)
{
    float v;
    if (meter == EM_METER_DEYE) {
        if (!s_deye.valid || !s_deye.online) return false;
        v = s_deye.pv_total;
    } else if (meter >= 0 && meter < ndev && meter < MB_MAX_DEVICES) {
        if (!s_dev[meter].connected) return false;
        v = s_dev[meter].pv_w;
    } else {
        return false;
    }
    if (!isfinite(v)) return false;
    *w = v > 0.0f ? v : 0.0f;    /* an inverter's own night draw is not production */
    return true;
}

/* Integrate measurement and uncalibrated forecast over the time since the
 * last tick, per meter group. The forecast is the slot mean, the measurement
 * a sample every ~10 s: over a day both sum to energies that compare. */
static void integrate(int64_t now)
{
    int64_t last = s_last_tick;
    s_last_tick  = now;
    if (last == 0 || now <= last || now - last > PV_MAX_GAP_S) return;
    double  h    = (double)(now - last) / 3600.0;
    int64_t tmid = last + (now - last) / 2;
    int     slot = (s_day_t0 && tmid >= s_day_t0) ? (int)((tmid - s_day_t0) / EM_SLOT_S) : -1;
    if (slot >= PV_MEAS_SLOTS) slot = -1;
    /* a plane whose meter changed starts its curve over */
    for (int j = 0; j < s_cfg.nplanes; j++)
        if (s_keep.meter[j] != s_cfg.plane[j].meter) keep_clear_plane(j);

    bool need_tcp = false, need_deye = false;
    for (int i = 0; i < s_cfg.nplanes; i++) {
        int8_t m = s_cfg.plane[i].meter;
        if (m == EM_METER_DEYE) need_deye = true;
        else if (m >= 0)        need_tcp  = true;
    }
    int ndev = 0;
    if (need_tcp)  ndev = modbus_tcp_get_device_live(s_dev, MB_MAX_DEVICES);
    if (need_deye) modbus_rtu_get_deye_live(&s_deye);

    for (int i = 0; i < s_cfg.nplanes; i++) {
        int8_t m = s_cfg.plane[i].meter;
        if (m == EM_METER_NONE || group_leader(i) != i) continue;
        double fc = 0.0, kwp = 0.0;
        bool   fc_ok = true;
        for (int j = i; j < s_cfg.nplanes; j++) {
            if (s_cfg.plane[j].meter != m) continue;
            float v = raw_at(j, tmid);
            if (isfinite(v)) fc += v; else fc_ok = false;
            kwp += s_cfg.plane[j].kwp;
        }
        float w;
        if (!meter_w(m, ndev, &w)) {
            /* Fronius inverters switch off after sunset and stop answering on
             * Modbus. When the forecast says night, a silent inverter makes
             * 0 W -- a measurement, not a gap. By day it stays a gap. */
            if (!(fc_ok && fc < PV_NIGHT_KW)) continue;
            w = 0.0f;
        }
        double e = w / 1000.0 * h;
        /* Per-plane display: one meter cannot tell its planes apart, so the
         * energy is split by their forecast share (east in the morning, west
         * in the evening), by kWp when there is no forecast. */
        for (int j = i; j < s_cfg.nplanes; j++) {
            if (s_cfg.plane[j].meter != m) continue;
            double share = (fc_ok && fc > 1e-6) ? raw_at(j, tmid) / fc
                         : (kwp > 0.0 ? s_cfg.plane[j].kwp / kwp : 0.0);
            s_meas_kwh[j] += e * share;
            if (slot >= 0) {
                s_keep.kwh[j][slot] += (float)(e * share);
                uint32_t sec = s_keep.sec[j][slot] + (uint32_t)(now - last);
                s_keep.sec[j][slot] = (uint16_t)(sec > EM_SLOT_S ? EM_SLOT_S : sec);
            }
        }
        if (fc_ok) {
            s_grp_meas_kwh[i] += e;
            s_grp_fc_kwh[i]   += fc * h;
        }
    }
}

/* Uncalibrated forecast energy of plane i in [from, to), kWh. */
static double raw_kwh(int i, int64_t from, int64_t to)
{
    double s = 0.0;
    for (int64_t t = from; t < to; t += EM_SLOT_S) {
        float v = raw_at(i, t);
        if (isfinite(v)) s += v * (EM_SLOT_S / 3600.0);
    }
    return s;
}

/* End of a day: move each measured group's factor a fifth of the way towards
 * measured/forecast. Only days with something to learn from count: a dull day
 * (<= 0.5 kWh forecast) is all noise, and a day with the meter offline half
 * the time would compare a sunny morning with a whole day's forecast. */
static void calibrate(int64_t day_t0)
{
    int64_t day_end = next_midnight(day_t0);
    bool    changed = false;
    for (int i = 0; i < s_cfg.nplanes; i++) {
        int8_t m = s_cfg.plane[i].meter;
        if (m == EM_METER_NONE || group_leader(i) != i) continue;
        double fc_day = 0.0;
        bool   ok     = true;
        for (int j = i; j < s_cfg.nplanes; j++) {
            if (s_cfg.plane[j].meter != m) continue;
            if (!raw_ok(j)) ok = false;
            fc_day += raw_kwh(j, day_t0, day_end);
        }
        double cov = s_grp_fc_kwh[i];
        if (!ok || fc_day <= PV_CAL_MIN_KWH || cov <= PV_CAL_MIN_COVER * fc_day || cov <= 0.0) {
            ESP_LOGI(TAG, "Feld %d: Tag zählt nicht (Prognose %.2f kWh, gemessen über %.2f kWh)",
                     i, fc_day, cov);
            continue;
        }
        float ratio = (float)(s_grp_meas_kwh[i] / cov);
        for (int j = i; j < s_cfg.nplanes; j++) {
            if (s_cfg.plane[j].meter != m) continue;
            s_cal[j] = clamp_cal(s_cal[j] + PV_CAL_ALPHA * (ratio - s_cal[j]));
            ESP_LOGI(TAG, "Feld %d: gemessen/Prognose %.3f -> Faktor %.3f", j, ratio, s_cal[j]);
        }
        changed = true;
    }
    if (changed) save_cal();
}

static void day_rollover(int64_t today_t0)
{
    if (s_day_t0 == today_t0) return;
    if (s_day_t0 != 0) calibrate(s_day_t0);
    reset_day_acc();
    s_day_t0 = today_t0;
    keep_open_day(today_t0);
}

/* ------------------------------ fetch ---------------------------------- */

/* Copy at most cap-1 bytes of s without leaving half a UTF-8 character at
 * the end: the error text ends up in JSON and on the display. */
static void utf8_copy(char *out, size_t cap, const char *s)
{
    snprintf(out, cap, "%s", s);
    size_t n = strlen(out), k = n;
    while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;   /* continuation bytes */
    if (k == 0) return;
    unsigned char lead = (unsigned char)out[k - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (k - 1) < need) out[k - 1] = '\0';
}

/* Fetch the weather for the location -- one request for all planes, the
 * model turns it into each plane's power (compute_raw). On failure the
 * previous weather stays: an hour-old forecast beats none. */
static bool fetch_all(int64_t t0, int n)
{
    char *buf = heap_caps_malloc(PV_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(s_err, sizeof(s_err), "Kein Speicher für die PV-Prognose");
        return false;
    }
    /* start/end_minutely_15 instead of forecast_days: the window then is
     * exactly our two LOCAL days (with the slot shift, the first value is
     * stamped t0 + 15 min), not two UTC days that miss the first local hours
     * of today and the last ones of tomorrow. The hourly snow depth covers
     * the hours those slots start in. */
    char from[20], to[20], hfrom[20], hto[20];
    fmt_utc(t0 + EM_SLOT_S, from);
    fmt_utc(t0 + (int64_t)n * EM_SLOT_S, to);
    fmt_utc(t0 - t0 % 3600, hfrom);
    fmt_utc(t0 + (int64_t)n * EM_SLOT_S, hto);

    static EXT_RAM_BSS_ATTR char url[PV_URL_CAP];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&minutely_15=shortwave_radiation,diffuse_radiation,direct_normal_irradiance,"
             "temperature_2m,wind_speed_10m&hourly=snow_depth&wind_speed_unit=ms"
             "&timezone=UTC&start_minutely_15=%s&end_minutely_15=%s&start_hour=%s&end_hour=%s",
             (double)s_cfg.lat, (double)s_cfg.lon, from, to, hfrom, hto);
    size_t    len = 0;
    int       st  = 0;
    esp_err_t e   = https_get(&(https_req_t){ .url = url }, buf, PV_BUF_CAP, &len, &st);
    bool      ok  = false;
    if (e != ESP_OK) {
        if (e == ESP_ERR_NO_MEM) {
            snprintf(s_err, sizeof(s_err), "Open-Meteo verschoben, Speicher gerade knapp");
        } else if (st == 0) {
            snprintf(s_err, sizeof(s_err), "Open-Meteo nicht erreichbar");
        } else if (e == ESP_ERR_INVALID_SIZE) {
            snprintf(s_err, sizeof(s_err), "Open-Meteo: Antwort zu groß");
        } else {
            /* A 400 carries {"error":true,"reason":"..."}: worth showing. */
            cJSON *r = st == 400 ? cJSON_ParseWithLength(buf, len) : NULL;
            const cJSON *why = cJSON_GetObjectItemCaseSensitive(r, "reason");
            if (cJSON_IsString(why)) {
                char reason[57];
                utf8_copy(reason, sizeof(reason), why->valuestring);
                snprintf(s_err, sizeof(s_err), "Open-Meteo: %s", reason);
            } else {
                snprintf(s_err, sizeof(s_err), "Open-Meteo: HTTP %d", st);
            }
            cJSON_Delete(r);
        }
        ESP_LOGW(TAG, "%s", s_err);
    } else {
        int got = parse_wx(buf, len, t0, n);
        if (got <= 0) {
            snprintf(s_err, sizeof(s_err), "Open-Meteo: Antwort unlesbar");
            ESP_LOGW(TAG, "%s (%u Bytes)", s_err, (unsigned)len);
        } else {
            memcpy(s_wx, s_wx_tmp, sizeof(s_wx));
            s_wx_t0 = t0;
            s_wx_n  = n;
            for (int i = 0; i < s_cfg.nplanes; i++) compute_raw(i);
            ESP_LOGI(TAG, "Wetter: %d Viertelstunden, %d Felder gerechnet", got, s_cfg.nplanes);
            ok = true;
        }
    }
    heap_caps_free(buf);
    return ok;
}

/* Fetch when due. Nothing at all without a location or a plane: there is
 * nothing to ask for, and the page says why. */
static void maybe_fetch(int64_t now, int64_t t0, int n)
{
    const char *pre = NULL;
    if (s_cfg.lat == 0.0f && s_cfg.lon == 0.0f) pre = "Standort nicht eingestellt";
    else if (s_cfg.nplanes == 0)                pre = "Keine PV-Felder eingestellt";
    if (pre) {
        snprintf(s_err, sizeof(s_err), "%s", pre);
        s_err_pre = true;
        return;
    }
    /* The hint goes as soon as its cause does, not only with the next fetch. */
    if (s_err_pre) { s_err[0] = '\0'; s_err_pre = false; }
    if (!s_req && now < s_next_fetch) return;
    s_req    = false;       /* a request arriving during the fetch stays set */
    s_err[0] = '\0';
    if (fetch_all(t0, n)) {
        s_fetched    = now;
        s_next_fetch = now + PV_FC_PERIOD_S;
        s_backoff    = 0;
    } else {
        s_next_fetch = now + BACKOFF_S[s_backoff];
        if (s_backoff < BACKOFF_N - 1) s_backoff++;
    }
}

/* ----------------------------- publish --------------------------------- */

static void publish(int64_t t0, int64_t t1, int n)
{
    int n1 = (int)((t1 - t0) / EM_SLOT_S);     /* slots of today: 92, 96 or 100 */
    portENTER_CRITICAL(&s_mux);
    s_pub.n       = s_cfg.nplanes;
    s_pub.fetched = s_fetched;
    s_pub.meas_t0 = s_keep.day_t0 == t0 ? t0 : 0;
    s_pub.meas_n  = (uint8_t)(n1 < PV_MEAS_SLOTS ? n1 : PV_MEAS_SLOTS);
    memcpy(s_pub.err, s_err, sizeof(s_pub.err));
    em_series_t *tot = &s_pub.total_kw;
    tot->t0 = t0;
    tot->n  = (uint16_t)n;
    for (int k = 0; k < EM_MAX_SLOTS; k++) tot->v[k] = k < n ? 0.0f : NAN;
    for (int i = 0; i < EM_MAX_PLANES; i++) {
        pv_plane_fc_t *o = &s_pub.p[i];
        if (i >= s_cfg.nplanes) { memset(o, 0, sizeof(*o)); continue; }
        memcpy(o->name, s_cfg.plane[i].name, sizeof(o->name));
        o->name[sizeof(o->name) - 1] = '\0';
        o->cal  = s_cal[i];
        o->kw.t0 = t0;
        o->kw.n  = (uint16_t)n;
        double d[2] = { 0.0, 0.0 };
        bool   known[2] = { false, false };
        for (int k = 0; k < EM_MAX_SLOTS; k++) {
            float v = k < n ? raw_at(i, t0 + (int64_t)k * EM_SLOT_S) : NAN;
            if (isfinite(v)) {
                v = fminf(v * s_cal[i], ac_kw(i));   /* a factor > 1 must not lift it past the limit */
                d[k >= n1] += v * (EM_SLOT_S / 3600.0);
                known[k >= n1] = true;
            }
            o->kw.v[k] = v;
            if (k < n) tot->v[k] = isfinite(v) ? tot->v[k] + v : NAN;  /* NAN sticks */
        }
        o->kwh_today      = known[0] ? (float)d[0] : NAN;
        o->kwh_tomorrow   = known[1] ? (float)d[1] : NAN;
        o->meas_kwh_today = s_cfg.plane[i].meter == EM_METER_NONE ? NAN : (float)s_meas_kwh[i];
        for (int k = 0; k < PV_MEAS_SLOTS; k++) {
            uint16_t sec = (k < n1 && s_keep.day_t0 == t0) ? s_keep.sec[i][k] : 0;
            o->meas_kwh[k] = sec ? s_keep.kwh[i][k] : NAN;
            o->meas_cov[k] = (uint8_t)((sec * 100u + EM_SLOT_S / 2) / EM_SLOT_S);
        }
    }
    if (s_cfg.nplanes == 0)
        for (int k = 0; k < n; k++) tot->v[k] = NAN;
    portEXIT_CRITICAL(&s_mux);
}

static void publish_err_only(void)
{
    portENTER_CRITICAL(&s_mux);
    memcpy(s_pub.err, s_err, sizeof(s_pub.err));
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------------ public --------------------------------- */

void pv_forecast_tick(int64_t now)
{
    if (!s_inited) {
        load_cal();
        s_inited = true;
    }
    em_cfg_get(&s_cfg);
    if (s_cfg.nplanes > EM_MAX_PLANES) s_cfg.nplanes = EM_MAX_PLANES;
    sync_planes();

    if (now < PV_FC_CLOCK_OK) {
        /* No local day without a clock: nothing to integrate, fetch or show. */
        snprintf(s_err, sizeof(s_err), "Uhrzeit noch nicht gestellt");
        s_err_pre   = true;
        s_last_tick = 0;
        publish_err_only();
        return;
    }
    integrate(now);                  /* the interval belongs to the day it started in */
    if (s_day_t0) keep_seal();
    int64_t t0 = local_midnight(now);
    day_rollover(t0);

    int64_t t1 = next_midnight(t0), t2 = next_midnight(t1);
    int     n  = (int)((t2 - t0) / EM_SLOT_S);
    if (n > EM_MAX_SLOTS) n = EM_MAX_SLOTS;  /* the day before DST ends: 196 -> 192,
                                              * losing tomorrow's last (night) hour */
    maybe_fetch(now, t0, n);
    if (s_wx_n)
        for (int i = 0; i < s_cfg.nplanes; i++)
            if (!raw_ok(i)) compute_raw(i);
    publish(t0, t1, n);
}

void pv_forecast_request(void) { s_req = true; }

void pv_forecast_get(pv_fc_t *out)
{
    portENTER_CRITICAL(&s_mux);
    memcpy(out, &s_pub, sizeof(*out));
    portEXIT_CRITICAL(&s_mux);
}

#include "harness.h"
#include "fakes.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* esp_heap_caps.h has no host fake. pv_forecast.c skips the include when
 * MALLOC_CAP_SPIRAM is already defined, so these lines stand in -- and count,
 * so a leaked fetch buffer shows up. */
#define MALLOC_CAP_SPIRAM (1u << 10)
static int f_allocs, f_frees;
static void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; f_allocs++; return malloc(n); }
static void  heap_caps_free(void *p) { if (p) f_frees++; free(p); }

/* The PV forecast runs unattended for months and learns from what it
 * measures. What can go wrong is quiet: a forecast a quarter hour late, a
 * factor that drifts on a day the meter was offline, a factor learnt for one
 * roof applied to another, a fetch loop hammering a rate-limited API. Open-
 * Meteo is simulated here from the request URL, the meters from the forecast
 * itself times a known factor -- so the expected factor is known exactly.
 *
 * The physics (sun, transposition, temperature, efficiency) is tested on its
 * own against a reference implementation of the operator's model. Everything
 * around it runs on a plain stand-in model (test_plane_kw): sun windows per
 * orientation, kWp x GHI/1000 x PR -- simple numbers the checks can name. */
#include "em_cfg.h"
struct pv_wx;
static float test_plane_kw(const em_plane_t *p, const struct pv_wx *w, int64_t t, float lat, float lon);
#define PV_FORECAST_PLANE_KW test_plane_kw
#include "pv_forecast.c"

/* ----------------------------- fakes ---------------------------------- */

static em_cfg_t f_cfg;
void em_cfg_get(em_cfg_t *out) { *out = f_cfg; }

static mb_dev_live_t  f_dev[MB_MAX_DEVICES];
static int            f_ndev;
static mb_deye_live_t f_deye;
int modbus_tcp_get_device_live(mb_dev_live_t *out, int max)
{
    int n = f_ndev < max ? f_ndev : max;
    memcpy(out, f_dev, (size_t)n * sizeof(*out));
    return n;
}
void modbus_rtu_get_deye_live(mb_deye_live_t *out) { *out = f_deye; }

/* Open-Meteo, generated from the URL: the stamps it asks for, one GHI (all
 * of it diffuse), one air temperature, no wind, a snow depth. */
static int         f_calls;
static char        f_url[EM_MAX_PLANES][PV_URL_CAP];   /* ring, by call number */
static int         f_fail_call;          /* this call (1-based) answers 503, 0 none */
static esp_err_t   f_http_err;
static int         f_http_status;
static const char *f_http_body;          /* non-NULL: answer this instead */
static float       f_gti, f_temp, f_snow_m;
static int         f_null_stamp_sod;     /* stamp (UTC second of day) answered null, -1 none */
static char        f_body[PV_BUF_CAP];

/* The stand-in model: sun windows per orientation (slot start, UTC second of
 * day), kWp x GHI/1000 x PR inside them. */
static float test_plane_kw(const em_plane_t *p, const struct pv_wx *w, int64_t t, float lat, float lon)
{
    (void)lat; (void)lon;
    if (!isfinite(w->ghi)) return NAN;
    int64_t slot = t - EM_SLOT_S / 2;
    int     sod  = (int)(((slot % 86400) + 86400) % 86400);
    int     from = 10 * 3600, to = 12 * 3600;                 /* south */
    if (p->azimuth == 90)  { from = 6 * 3600;  to = 8 * 3600; }   /* east */
    if (p->azimuth == 270) { from = 14 * 3600; to = 16 * 3600; }  /* west */
    if (p->azimuth == 0)   from = to = 0;                          /* north: never */
    return sod >= from && sod < to ? p->kwp * w->ghi / 1000.0f * PV_FC_PR : 0.0f;
}

static const char *param(const char *url, const char *key)
{
    const char *p = strstr(url, key);
    return p ? p + strlen(key) : NULL;
}

static size_t f_series(size_t o, const char *name, int64_t from, int64_t to, float v, bool nulls)
{
    o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, ",\"%s\":[", name);
    for (int64_t t = from; t <= to; t += EM_SLOT_S) {
        bool null = nulls && (int)(((t % 86400) + 86400) % 86400) == f_null_stamp_sod;
        if (null) o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "%snull", t == from ? "" : ",");
        else      o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "%s%.2f", t == from ? "" : ",", (double)v);
    }
    return o + (size_t)snprintf(f_body + o, sizeof(f_body) - o, "]");
}

esp_err_t https_get(const https_req_t *rq, char *buf, size_t cap, size_t *len, int *status)
{
    snprintf(f_url[f_calls % EM_MAX_PLANES], PV_URL_CAP, "%s", rq->url);
    f_calls++;
    *status = f_http_status;
    if (f_calls == f_fail_call) { *status = 503; buf[0] = '\0'; *len = 0; return ESP_FAIL; }
    if (f_http_err != ESP_OK || f_http_body) {
        const char *b = f_http_body ? f_http_body : "";
        snprintf(buf, cap, "%s", b);
        *len = strlen(buf);
        return f_http_err;
    }
    int64_t from, to, hfrom, hto;
    CHECK(parse_utc(param(rq->url, "start_minutely_15="), &from));
    CHECK(parse_utc(param(rq->url, "end_minutely_15="), &to));
    CHECK(parse_utc(param(rq->url, "start_hour="), &hfrom));
    CHECK(parse_utc(param(rq->url, "end_hour="), &hto));
    size_t o = 0;
    o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "{\"minutely_15\":{\"time\":[");
    for (int64_t t = from; t <= to; t += EM_SLOT_S) {
        char s[20];
        fmt_utc(t, s);
        o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "%s\"%s\"", t == from ? "" : ",", s);
    }
    o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "]");
    o = f_series(o, "shortwave_radiation", from, to, f_gti, true);
    o = f_series(o, "diffuse_radiation", from, to, f_gti, true);
    o = f_series(o, "direct_normal_irradiance", from, to, 0.0f, false);
    o = f_series(o, "temperature_2m", from, to, f_temp, false);
    o = f_series(o, "wind_speed_10m", from, to, 0.0f, false);
    o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "},\"hourly\":{\"time\":[");
    for (int64_t t = hfrom; t <= hto; t += 3600) {
        char s[20];
        fmt_utc(t, s);
        o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "%s\"%s\"", t == hfrom ? "" : ",", s);
    }
    o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "],\"snow_depth\":[");
    for (int64_t t = hfrom; t <= hto; t += 3600)
        o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "%s%.3f", t == hfrom ? "" : ",", (double)f_snow_m);
    o += (size_t)snprintf(f_body + o, sizeof(f_body) - o, "]}}");
    CHECK(o < cap);
    memcpy(buf, f_body, o + 1);
    *len = o;
    return ESP_OK;
}

/* ----------------------------- helpers -------------------------------- */

/* The model's numbers, derived from its constant so they follow it: 5 kWp at
 * 1000 W/m^2 and a 25 C cell, and the 2 h of sun in each simulated day. */
#define PR   ((double)PV_FC_PR)
#define KW5  (5.0 * PR)
#define KWH5 (2.0 * KW5)

static pv_fc_t g_fc;   /* ~5.8 kB: static, like the callers in the firmware */

static int64_t loc(int y, int mo, int d, int h, int mi)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
                     .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return (int64_t)mktime(&tm);
}

static em_plane_t plane(const char *name, float kwp, int tilt, int az, int meter)
{
    em_plane_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "%s", name);
    p.kwp = kwp; p.tilt = (int16_t)tilt; p.azimuth = (int16_t)az; p.meter = (int8_t)meter;
    return p;
}

/* Forget everything the module holds in RAM -- a reboot. NVS stays. */
static void restart(void)
{
    s_inited = false;
    memset(s_raw, 0, sizeof(s_raw));
    memset(s_raw_sig, 0, sizeof(s_raw_sig));
    s_wx_n = 0; s_wx_t0 = 0;                  /* the weather is RAM too */
    memset(s_cal, 0, sizeof(s_cal));
    memset(s_sig, 0, sizeof(s_sig));
    s_nsig = 0;
    s_fetched = s_next_fetch = 0;
    s_backoff = 0;
    s_req = false;
    s_err[0] = '\0';
    s_err_pre = false;
    s_day_t0 = s_last_tick = 0;
    reset_day_acc();
    memset(&s_pub, 0, sizeof(s_pub));
}

static void reset(void)
{
    restart();
    memset(&s_keep, 0, sizeof(s_keep));      /* power cut: the kept PSRAM is gone too */
    fake_nvs_reset_em();
    fake_nvs_fail = false;
    memset(&f_cfg, 0, sizeof(f_cfg));
    f_cfg.ver = EM_CFG_VER;
    f_cfg.lat = 48.8f; f_cfg.lon = 9.2f;
    f_cfg.nplanes = 1;
    f_cfg.plane[0] = plane("Süd", 5.0f, 30, 180, 0);
    memset(f_dev, 0, sizeof(f_dev));
    f_ndev = 2;
    memset(&f_deye, 0, sizeof(f_deye));
    f_calls = 0;
    f_fail_call = 0;
    memset(f_url, 0, sizeof(f_url));
    f_http_err = ESP_OK; f_http_status = 200; f_http_body = NULL;
    f_gti = 1000.0f; f_temp = -6.0f; f_snow_m = 0.0f;
    f_null_stamp_sod = -1;
    f_allocs = f_frees = 0;
}

static void tick(int64_t t) { pv_forecast_tick(t); pv_forecast_get(&g_fc); }

/* Uncalibrated forecast of a meter group at t, kW -- what the simulated
 * meter multiplies by its factor. */
static float group_fc(int meter, int64_t t)
{
    float s = 0.0f;
    for (int i = 0; i < f_cfg.nplanes; i++)
        if (f_cfg.plane[i].meter == meter) { float v = raw_at(i, t); if (isfinite(v)) s += v; }
    return s;
}

/* One local day in 30-s ticks, ending before midnight; the next tick (the
 * caller's, or the next day's first) rolls over and calibrates. Meter 0 reads
 * fac0 x its forecast, meter 1 fac1 x, the Deye facd x. Meter 0 drops out
 * after UTC second-of-day `cut0` (-1 = never). */
#define STEP 30
static void run_day(int64_t day_t0, float fac0, float fac1, float facd, int cut0)
{
    int64_t end = next_midnight(day_t0);
    for (int64_t t = day_t0 + STEP / 2; t < end; t += STEP) {
        int64_t tm  = t - STEP / 2;                  /* the tick integrates around here */
        int     sod = (int)(tm % 86400);
        f_dev[0].connected = cut0 < 0 || sod < cut0;
        f_dev[0].pv_w      = fac0 * group_fc(0, tm) * 1000.0f;
        f_dev[1].connected = true;
        f_dev[1].pv_w      = fac1 * group_fc(1, tm) * 1000.0f;
        f_deye.valid = f_deye.online = true;
        f_deye.pv_total    = facd * group_fc(EM_METER_DEYE, tm) * 1000.0f;
        tick(t);
    }
}

/* ------------------------------ tests --------------------------------- */

/* The operator's model against its reference implementation (Python, the
 * formulas as given, double precision): within 0.5 %. */
static void test_the_model(void)
{
    static const struct { int64_t t; double cosz, az; } SUN[] = {
        { 1782040920LL, 0.90314, 178.387 },  /* 2026-06-21 11:22 UTC: 64.6 deg up */
        { 1797852600LL, 0.30378, 182.043 },  /* 2026-12-21 11:30 UTC: 17.7 deg up */
        { 1773986400LL, 0.07298, 95.664 },   /* 2026-03-20 06:00 UTC: sunrise, east */
        { 1792681200LL, 0.20106, 238.242 },  /* 2026-10-22 15:00 UTC: afternoon, mirrored */
    };
    for (size_t i = 0; i < sizeof(SUN) / sizeof(SUN[0]); i++) {
        float cz, az;
        sun_pos(SUN[i].t, 48.8705f, 9.0759f, &cz, &az);
        CHECK_F(cz, SUN[i].cosz, 2e-4);
        CHECK_F(az, SUN[i].az, 0.05);
    }

    static const struct {
        const char *what; float kwp; int tilt, az; int64_t t;
        float ghi, dhi, dni, tair, wind, snow_cm; double kw;
    } C[] = {
        { "Ost sommer vormittag", 6.6f, 30, 110, 1782025650LL, 600, 150, 700, 18, 2, 0, 4.63205 },
        { "West sommer nachmittag", 6.6f, 30, 290, 1782054450LL, 550, 160, 650, 26, 3, 0, 4.29418 },
        { "Sued fassade winter mittag", 8.5f, 90, 200, 1797851250LL, 350, 80, 750, -2, 1, 0, 6.57796 },
        { "Sued fassade sommer mittag", 8.5f, 90, 200, 1782040050LL, 850, 150, 800, 25, 2, 0, 3.18536 },
        { "Ost bedeckt", 6.6f, 30, 110, 1792663650LL, 200, 200, 0, 10, 4, 0, 1.11279 },
        { "Ost schnee 3cm", 6.6f, 30, 110, 1768475250LL, 300, 100, 500, -3, 1, 3, 0.70212 },
        { "Fassade schnee 3cm", 8.5f, 90, 200, 1768475250LL, 300, 100, 500, -3, 1, 3, 5.26474 },
        { "West sonne hinter modul", 6.6f, 30, 290, 1782018450LL, 150, 90, 300, 12, 1, 0, 0.32677 },
        { "Nacht", 6.6f, 30, 110, 1782079650LL, 0, 0, 0, 12, 1, 0, 0.00000 },
    };
    for (size_t i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
        em_plane_t p = plane(C[i].what, C[i].kwp, C[i].tilt, C[i].az, EM_METER_NONE);
        pv_wx_t w = { C[i].ghi, C[i].dhi, C[i].dni, C[i].tair, C[i].wind, C[i].snow_cm };
        float kw = pv_plane_kw(&p, &w, C[i].t, 48.8705f, 9.0759f);
        CHECK_F(kw, C[i].kw, C[i].kw * 0.005 + 1e-4);
        if (fabs(kw - C[i].kw) > C[i].kw * 0.005 + 1e-4) printf("    %s: %.5f statt %.5f\n", C[i].what, kw, C[i].kw);
    }

    /* the facade's winter noon makes more than its summer noon (low sun) */
    CHECK(C[2].kw > C[3].kw);
    /* snow: the roof plane is derated (x0.3 at 3 cm), the facade is not */
    em_plane_t ost = plane("Ost", 6.6f, 30, 110, EM_METER_NONE);
    pv_wx_t s0 = { 300, 100, 500, -3, 1, 0 }, s3 = { 300, 100, 500, -3, 1, 3 };
    float k0 = pv_plane_kw(&ost, &s0, 1768475250LL, 48.8705f, 9.0759f);
    float k3 = pv_plane_kw(&ost, &s3, 1768475250LL, 48.8705f, 9.0759f);
    CHECK(k3 < k0 * 0.31f && k3 > k0 * 0.29f);    /* x0.3, plus a little more albedo */

    /* unknown stays unknown; missing temperature/wind still give a value */
    pv_wx_t un = { NAN, 100, 500, 10, 1, 0 };
    CHECK(isnan(pv_plane_kw(&ost, &un, 1782025650LL, 48.8705f, 9.0759f)));
    pv_wx_t nt = { 600, 150, 700, NAN, NAN, 0 };
    CHECK(pv_plane_kw(&ost, &nt, 1782025650LL, 48.8705f, 9.0759f) > 3.0f);
    pv_wx_t nd = { 600, 150, NAN, 18, 2, 0 };     /* no DNI: diffuse only, less */
    CHECK(pv_plane_kw(&ost, &nd, 1782025650LL, 48.8705f, 9.0759f) < C[0].kw);
}

/* One request for all planes, with everything the model needs, and the
 * local-day window. */
static void test_the_request(void)
{
    reset();
    f_cfg.nplanes = 3;
    f_cfg.plane[0] = plane("Ost", 4.0f, 25, 90, EM_METER_NONE);
    f_cfg.plane[1] = plane("West", 4.0f, 25, 270, EM_METER_NONE);
    f_cfg.plane[2] = plane("Süd", 4.0f, 90, 180, EM_METER_NONE);
    tick(loc(2026, 6, 15, 9, 0));
    CHECK_I(f_calls, 1);
    CHECK(strstr(f_url[0], "latitude=48.8000&longitude=9.2000") != NULL);
    CHECK(strstr(f_url[0], "minutely_15=shortwave_radiation,diffuse_radiation,direct_normal_irradiance,"
                           "temperature_2m,wind_speed_10m&") != NULL);
    CHECK(strstr(f_url[0], "hourly=snow_depth&") != NULL);
    CHECK(strstr(f_url[0], "wind_speed_unit=ms") != NULL);
    CHECK(strstr(f_url[0], "timezone=UTC") != NULL);
    CHECK(strstr(f_url[0], "azimuth") == NULL);
    /* local 2026-06-15 00:00 CEST = 06-14 22:00 UTC; first stamp is the end
     * of the first slot, last one the end of tomorrow. */
    CHECK(strstr(f_url[0], "start_minutely_15=2026-06-14T22:15&") != NULL);
    CHECK(strstr(f_url[0], "end_minutely_15=2026-06-16T22:00&") != NULL);
    CHECK(strstr(f_url[0], "start_hour=2026-06-14T22:00&") != NULL);
    CHECK(strlen(f_url[0]) < PV_URL_CAP - 1);
    for (int i = 0; i < 3; i++) CHECK(raw_ok(i));

    /* A changed orientation is recomputed from the stored weather: no fetch. */
    f_cfg.plane[0].azimuth = 270;
    tick(loc(2026, 6, 15, 9, 1));
    CHECK_I(f_calls, 1);
    CHECK(raw_ok(0));
    CHECK_F(g_fc.p[0].kw.v[68], 4.0 * PR, 1e-3);   /* now in the west window: 16:00 CEST */
}

static void test_parsing_nulls_and_the_slot_shift(void)
{
    /* t0 = 2026-06-15 10:00 UTC. The value stamped 10:15 is the mean of
     * 10:00..10:15 -> slot 0; 10:00 belongs to the slot before t0 -> dropped;
     * 11:00 would be slot 3 = past n. Snow is hourly, in metres. */
    int64_t t0;
    CHECK(parse_utc("2026-06-15T10:00", &t0));
    CHECK_I(t0, 1781517600LL);
    const char *js =
        "{\"latitude\":48.8,\"minutely_15\":{"
        "\"time\":[\"2026-06-15T10:00\",\"2026-06-15T10:15\",\"2026-06-15T10:30\","
                  "\"2026-06-15T10:45\",\"2026-06-15T11:00\"],"
        "\"shortwave_radiation\":[900,1000,null,500,700],"
        "\"diffuse_radiation\":[100,200,100,null,100],"
        "\"direct_normal_irradiance\":[1,2,3,4,5],"
        "\"temperature_2m\":[20,-6,10,null,10],"
        "\"wind_speed_10m\":[1,2,3,4,5]},"
        "\"hourly\":{\"time\":[\"2026-06-15T10:00\"],\"snow_depth\":[0.03]}}";
    int got = parse_wx(js, strlen(js), t0, 3);
    CHECK_I(got, 3);
    CHECK_F(s_wx_tmp[0].ghi, 1000, 0);           /* stamp 10:15 */
    CHECK_F(s_wx_tmp[0].dhi, 200, 0);
    CHECK_F(s_wx_tmp[0].dni, 2, 0);
    CHECK_F(s_wx_tmp[0].tair, -6, 0);
    CHECK_F(s_wx_tmp[0].wind, 2, 0);
    CHECK_F(s_wx_tmp[0].snow_cm, 3.0, 1e-4);     /* 0.03 m */
    CHECK(isnan(s_wx_tmp[1].ghi));               /* stamp 10:30: null */
    CHECK(isnan(s_wx_tmp[2].dhi));               /* stamp 10:45 */
    CHECK(isnan(s_wx_tmp[2].tair));
    CHECK(isnan(s_wx_tmp[3].ghi));               /* never written past n */
    CHECK(isnan(s_wx_tmp[EM_MAX_SLOTS - 1].ghi));

    /* Not usable: not JSON, no arrays, an error object. */
    CHECK(parse_wx("<html>", 6, t0, 3) < 0);
    const char *e1 = "{\"minutely_15\":{\"time\":[]}}";
    CHECK(parse_wx(e1, strlen(e1), t0, 3) < 0);
    const char *e2 = "{\"error\":true,\"reason\":\"x\"}";
    CHECK(parse_wx(e2, strlen(e2), t0, 3) < 0);
    /* Stamps that miss the grid (off by 5 min) are skipped, not rounded. */
    const char *e3 = "{\"minutely_15\":{\"time\":[\"2026-06-15T10:20\"],"
                     "\"shortwave_radiation\":[100],\"diffuse_radiation\":[100]}}";
    CHECK_I(parse_wx(e3, strlen(e3), t0, 3), 0);
}

static void test_series_and_daily_sums(void)
{
    reset();
    f_cfg.nplanes = 2;
    f_cfg.plane[1] = plane("Nord", 1.0f, 30, 0, EM_METER_NONE);   /* north: no sun here */
    f_null_stamp_sod = 3 * 3600;      /* stamp 03:00 UTC = slot 02:45 UTC = local 04:45 */
    tick(loc(2026, 6, 15, 9, 0));
    CHECK_I(f_calls, 1);                            /* one request for both planes */
    CHECK_I(g_fc.n, 2);
    CHECK(strcmp(g_fc.p[0].name, "Süd") == 0);
    CHECK_I(g_fc.p[0].kw.t0, loc(2026, 6, 15, 0, 0));
    CHECK_I(g_fc.p[0].kw.t0 % EM_SLOT_S, 0);
    CHECK_I(g_fc.p[0].kw.n, 192);
    CHECK_I(g_fc.fetched, loc(2026, 6, 15, 9, 0));
    CHECK(g_fc.err[0] == '\0');
    /* Sun 10:00..12:00 UTC = 12:00..14:00 CEST = slots 48..55. */
    CHECK_F(g_fc.p[0].kw.v[47], 0.0, 0);
    CHECK_F(g_fc.p[0].kw.v[48], KW5, 1e-3);
    CHECK_F(g_fc.p[0].kw.v[55], KW5, 1e-3);
    CHECK_F(g_fc.p[0].kw.v[56], 0.0, 0);
    CHECK_F(g_fc.p[0].kw.v[96 + 48], KW5, 1e-3);    /* tomorrow */
    CHECK(isnan(g_fc.p[0].kw.v[19]));               /* the null */
    CHECK_F(g_fc.p[0].kw.v[0], 0.0, 0);             /* first local slot is covered */
    CHECK_F(g_fc.p[0].kw.v[191], 0.0, 0);           /* last one too */
    CHECK_F(g_fc.p[0].kwh_today, KWH5, 1e-2);        /* 8 slots x KW5 kW x 0.25 h */
    CHECK_F(g_fc.p[0].kwh_tomorrow, KWH5, 1e-2);
    CHECK_F(g_fc.p[1].kwh_today, 0.0, 0);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
    CHECK_F(g_fc.total_kw.v[50], KW5, 1e-3);
    CHECK(isnan(g_fc.total_kw.v[19]));              /* one plane unknown -> sum unknown */
    CHECK_I(g_fc.total_kw.t0, g_fc.p[0].kw.t0);
    CHECK(isnan(g_fc.p[1].meas_kwh_today));         /* not measured */
    CHECK_F(g_fc.p[0].meas_kwh_today, 0.0, 0);      /* measured, nothing yet */
    CHECK_I(f_allocs, f_frees);

    /* Next day without a fetch: the series moves along, today is
     * yesterday's tomorrow, tomorrow is unknown. */
    f_http_err = ESP_FAIL; f_http_status = 503;
    tick(loc(2026, 6, 16, 0, 10));
    CHECK_I(g_fc.p[0].kw.t0, loc(2026, 6, 16, 0, 0));
    CHECK_F(g_fc.p[0].kw.v[48], KW5, 1e-3);
    CHECK_F(g_fc.p[0].kwh_today, KWH5, 1e-2);
    CHECK(isnan(g_fc.p[0].kwh_tomorrow));
    CHECK(isnan(g_fc.p[0].kw.v[100]));
}

static void test_dst_days(void)
{
    /* 2026-10-25 has 100 quarter hours: today 100 + tomorrow 96 = 196 slots,
     * of which 192 fit. Tomorrow loses its last hour (night). */
    reset();
    tick(loc(2026, 10, 25, 9, 0));
    CHECK_I(g_fc.p[0].kw.t0, loc(2026, 10, 25, 0, 0));
    CHECK_I(g_fc.p[0].kw.t0, 1792879200LL);      /* 2026-10-24 22:00 UTC */
    CHECK_I(g_fc.p[0].kw.n, 192);
    CHECK(strstr(f_url[0], "start_minutely_15=2026-10-24T22:15&") != NULL);
    CHECK(strstr(f_url[0], "end_minutely_15=2026-10-26T22:00") != NULL);   /* t0 + 192 slots */
    /* Sun 10..12 UTC = 11..13 CET. Local midnight was 22:00 UTC, so 11:00
     * CET is 12 real hours later -> slot 48, though the clock says 11. */
    CHECK_F(g_fc.p[0].kw.v[47], 0.0, 0);
    CHECK_F(g_fc.p[0].kw.v[48], KW5, 1e-3);
    CHECK_F(g_fc.p[0].kw.v[100 + 44], KW5, 1e-3);   /* tomorrow 11:00 CET */
    CHECK_F(g_fc.p[0].kwh_today, KWH5, 1e-2);
    CHECK_F(g_fc.p[0].kwh_tomorrow, KWH5, 1e-2);

    /* The day before: 96 + 100 -> also capped at 192. */
    reset();
    tick(loc(2026, 10, 24, 9, 0));
    CHECK_I(g_fc.p[0].kw.n, 192);
    CHECK_F(g_fc.p[0].kwh_tomorrow, KWH5, 1e-2);

    /* Spring: 2026-03-29 has 92 quarter hours. */
    reset();
    tick(loc(2026, 3, 29, 9, 0));
    CHECK_I(g_fc.p[0].kw.n, 92 + 96);
    CHECK(isnan(g_fc.p[0].kw.v[188]));
}

static void test_calibration_one_day(void)
{
    reset();
    int64_t d = loc(2026, 6, 15, 0, 0);
    run_day(d, 0.8f, 0.0f, 0.0f, -1);
    CHECK_F(g_fc.p[0].meas_kwh_today, 0.8 * KWH5, 0.02);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
    tick(next_midnight(d) + 15);
    /* 1 + 0.2 x (0.8 - 1) */
    CHECK_F(g_fc.p[0].cal, 0.96, 1e-3);
    CHECK_F(g_fc.p[0].kwh_today, 0.96 * KWH5, 0.02);
    CHECK_F(g_fc.p[0].kw.v[48], 0.96 * KW5, 1e-3);
    CHECK_F(g_fc.p[0].meas_kwh_today, 0.0, 1e-3);    /* a new day */
    CHECK_I(f_allocs, f_frees);
}

static void test_calibration_is_an_ema(void)
{
    reset();
    int64_t d = loc(2026, 6, 15, 0, 0);
    for (int i = 0; i < 3; i++) { run_day(d, 0.5f, 0, 0, -1); d = next_midnight(d); }
    tick(d + 15);
    CHECK_F(g_fc.p[0].cal, 0.756, 2e-3);    /* 1 -> 0.9 -> 0.82 -> 0.756 */
}

static void store_cal(float cal)
{
    static pv_cal_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = PV_CAL_VER; r.n = 1;
    r.p[0].sig = plane_sig(&f_cfg.plane[0]);
    r.p[0].cal = cal;
    CHECK_I(nvs_store_set_pv_cal(&r, sizeof(r)), ESP_OK);
}

static void test_calibration_clamps(void)
{
    reset();
    store_cal(1.45f);
    int64_t d = loc(2026, 6, 15, 0, 0);
    run_day(d, 3.0f, 0, 0, -1);        /* a meter that reads 3x: 1.45 + 0.31 */
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 1.5, 1e-6);

    reset();
    store_cal(0.32f);
    run_day(d, 0.0f, 0, 0, -1);        /* full coverage, nothing produced */
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 0.3, 1e-6);

    /* A stored factor out of range or broken is clamped / dropped on load. */
    reset();
    store_cal(7.0f);
    tick(d + 15);
    CHECK_F(g_fc.p[0].cal, 1.5, 1e-6);
    reset();
    store_cal(NAN);
    tick(d + 15);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
}

static void test_coverage_and_size_rules(void)
{
    int64_t d = loc(2026, 6, 15, 0, 0);
    /* Sun 10:00..12:00 UTC evenly: offline from 11:12 = 60 % covered. */
    reset();
    run_day(d, 0.5f, 0, 0, 11 * 3600 + 12 * 60);
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);

    /* Offline from 11:48 = 90 % covered: counts, and the ratio compares the
     * covered part only (not 0.9 x 0.8 against the whole day). */
    reset();
    run_day(d, 0.8f, 0, 0, 11 * 3600 + 48 * 60);
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 0.96, 1e-3);

    /* A dull day: 0.1 kWp makes 0.18 kWh -- noise, not information. */
    reset();
    f_cfg.plane[0].kwp = 0.1f;
    run_day(d, 0.5f, 0, 0, -1);
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);

    /* No forecast at all (location missing): nothing to compare with. */
    reset();
    f_cfg.lat = f_cfg.lon = 0.0f;
    run_day(d, 0.5f, 0, 0, -1);
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
    CHECK_I(f_calls, 0);

    /* A Deye that never answers counts nothing. */
    reset();
    f_cfg.plane[0].meter = EM_METER_DEYE;
    int64_t end = next_midnight(d);
    for (int64_t t = d + 15; t < end; t += STEP) {
        f_deye.valid = true; f_deye.online = false;
        f_deye.pv_total = 3000.0f;
        tick(t);
    }
    CHECK_F(g_fc.p[0].meas_kwh_today, 0.0, 0);
    tick(end + 15);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
}

static void test_shared_meter_groups(void)
{
    /* East and west on one Fronius (meter 0), south on the Deye, one plane
     * not measured. The Fronius reads 0.5 x east + 1.0 x west: the group
     * ratio is 0.75 and goes to BOTH planes. */
    reset();
    f_cfg.nplanes = 4;
    f_cfg.plane[0] = plane("Ost", 5.0f, 30, 90, 0);
    f_cfg.plane[1] = plane("Süd", 5.0f, 30, 180, EM_METER_DEYE);
    f_cfg.plane[2] = plane("West", 5.0f, 30, 270, 0);
    f_cfg.plane[3] = plane("Garage", 2.0f, 10, 180, EM_METER_NONE);
    int64_t d   = loc(2026, 6, 15, 0, 0);
    int64_t end = next_midnight(d);
    for (int64_t t = d + 15; t < end; t += STEP) {
        int64_t tm = t - STEP / 2;
        f_dev[0].connected = true;
        f_dev[0].pv_w = (0.5f * raw_at(0, tm) + 1.0f * raw_at(2, tm)) * 1000.0f;
        f_deye.valid = f_deye.online = true;
        f_deye.pv_total = 1.2f * raw_at(1, tm) * 1000.0f;
        if (!isfinite(f_dev[0].pv_w)) f_dev[0].pv_w = 0.0f;
        if (!isfinite(f_deye.pv_total)) f_deye.pv_total = 0.0f;
        tick(t);
    }
    /* The one meter's energy is split by forecast share: east gets the
     * morning, west the afternoon. */
    CHECK_F(g_fc.p[0].meas_kwh_today, 0.5 * KWH5, 0.02);
    CHECK_F(g_fc.p[2].meas_kwh_today, 1.0 * KWH5, 0.02);
    CHECK_F(g_fc.p[1].meas_kwh_today, 1.2 * KWH5, 0.02);
    CHECK(isnan(g_fc.p[3].meas_kwh_today));
    tick(end + 15);
    CHECK_F(g_fc.p[0].cal, 0.95, 1e-3);    /* 1 + 0.2 x (0.75 - 1) */
    CHECK_F(g_fc.p[2].cal, 0.95, 1e-3);
    CHECK_F(g_fc.p[1].cal, 1.04, 1e-3);    /* 1 + 0.2 x (1.2 - 1)  */
    CHECK_F(g_fc.p[3].cal, 1.0, 0);
    CHECK_F(g_fc.p[3].kwh_today, 2.0 * PR * 2.0, 0.02);   /* uncalibrated */
}

static void test_geometry_change_resets_and_restart_keeps(void)
{
    reset();
    f_cfg.nplanes = 2;
    f_cfg.plane[1] = plane("Dach", 3.0f, 30, 180, EM_METER_DEYE);
    int64_t d = loc(2026, 6, 15, 0, 0);
    run_day(d, 0.8f, 0, 0.8f, -1);
    int64_t t = next_midnight(d) + 15;
    tick(t);
    CHECK_F(g_fc.p[0].cal, 0.96, 1e-3);
    CHECK_F(g_fc.p[1].cal, 0.96, 1e-3);

    /* A reboot: the factors come back from NVS. */
    restart();
    tick(t + 10);
    CHECK_F(g_fc.p[0].cal, 0.96, 1e-3);
    CHECK_F(g_fc.p[1].cal, 0.96, 1e-3);

    /* Renaming keeps a factor; turning the roof does not. */
    snprintf(f_cfg.plane[1].name, sizeof(f_cfg.plane[1].name), "Scheune");
    f_cfg.plane[0].azimuth = 200;
    int calls = f_calls;
    tick(t + 20);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
    CHECK_F(g_fc.p[1].cal, 0.96, 1e-3);
    CHECK(strcmp(g_fc.p[1].name, "Scheune") == 0);
    CHECK_I(f_calls, calls);                  /* no fetch: recomputed from the weather */
    CHECK(raw_ok(0));

    /* ... and the reset is persisted. */
    restart();
    tick(t + 30);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
    CHECK_F(g_fc.p[1].cal, 0.96, 1e-3);

    /* Other knobs of the signature: kWp, tilt, meter. */
    f_cfg.plane[1].meter = 3;
    tick(t + 40);
    CHECK_F(g_fc.p[1].cal, 1.0, 0);

    /* Removing a plane and adding another one in its place: a new plane. */
    reset();
    f_cfg.nplanes = 2;
    f_cfg.plane[1] = plane("Dach", 3.0f, 30, 180, EM_METER_DEYE);
    run_day(d, 0.8f, 0, 0.8f, -1);
    tick(t);
    CHECK_F(g_fc.p[1].cal, 0.96, 1e-3);
    f_cfg.nplanes = 1;
    tick(t + 10);
    CHECK_I(g_fc.n, 1);
    f_cfg.nplanes = 2;
    tick(t + 20);
    CHECK_F(g_fc.p[1].cal, 1.0, 0);

    /* A record of another version is not trusted. */
    reset();
    static pv_cal_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = PV_CAL_VER + 1; r.n = 1; r.p[0].sig = plane_sig(&f_cfg.plane[0]); r.p[0].cal = 0.5f;
    nvs_store_set_pv_cal(&r, sizeof(r));
    tick(t);
    CHECK_F(g_fc.p[0].cal, 1.0, 0);
}

static void test_schedule_and_backoff(void)
{
    reset();
    int64_t t = loc(2026, 6, 15, 9, 0);
    tick(t);
    CHECK_I(f_calls, 1);
    tick(t + 10);
    tick(t + 3599);
    CHECK_I(f_calls, 1);
    tick(t + 3600);
    CHECK_I(f_calls, 2);

    /* Failure: 5, 10, 20, then 60 minutes. The old forecast stays. */
    t += 3600;
    f_http_err = ESP_FAIL; f_http_status = 500;
    tick(t + 3600);
    CHECK_I(f_calls, 3);
    CHECK(strstr(g_fc.err, "HTTP 500") != NULL);
    CHECK_F(g_fc.p[0].kw.v[48], KW5, 1e-3);
    CHECK_I(g_fc.fetched, t);
    int64_t f0 = t + 3600;
    tick(f0 + 299);              CHECK_I(f_calls, 3);
    tick(f0 + 300);              CHECK_I(f_calls, 4);
    tick(f0 + 300 + 599);        CHECK_I(f_calls, 4);
    tick(f0 + 900);              CHECK_I(f_calls, 5);
    tick(f0 + 900 + 1199);       CHECK_I(f_calls, 5);
    tick(f0 + 2100);             CHECK_I(f_calls, 6);
    tick(f0 + 2100 + 3599);      CHECK_I(f_calls, 6);
    tick(f0 + 5700);             CHECK_I(f_calls, 7);
    tick(f0 + 5700 + 3600);      CHECK_I(f_calls, 8);    /* stays at 60 min */
    CHECK_I(f_allocs, f_frees);

    /* Success resets the backoff and the error. */
    f_http_err = ESP_OK; f_http_status = 200;
    int64_t f1 = f0 + 5700 + 7200;
    tick(f1);                    CHECK_I(f_calls, 9);
    CHECK(g_fc.err[0] == '\0');
    CHECK_I(g_fc.fetched, f1);
    f_http_err = ESP_FAIL; f_http_status = 500;
    tick(f1 + 3600);             CHECK_I(f_calls, 10);
    tick(f1 + 3600 + 300);       CHECK_I(f_calls, 11);   /* 5 min again */

    /* A request (settings saved, button) fetches at the next tick. */
    f_http_err = ESP_OK; f_http_status = 200;
    pv_forecast_request();
    tick(f1 + 3600 + 310);       CHECK_I(f_calls, 12);
    tick(f1 + 3600 + 320);       CHECK_I(f_calls, 12);
}

static void test_error_texts(void)
{
    int64_t t = loc(2026, 6, 15, 9, 0);
    reset();
    f_http_err = ESP_FAIL; f_http_status = 0;
    tick(t);
    CHECK(strstr(g_fc.err, "nicht erreichbar") != NULL);
    CHECK_I(g_fc.fetched, 0);

    reset();
    f_http_err = ESP_ERR_NO_MEM; f_http_status = 0;
    tick(t);
    CHECK(strstr(g_fc.err, "verschoben, Speicher gerade knapp") != NULL);
    reset();
    f_http_err = ESP_FAIL; f_http_status = 0;
    tick(t);
    CHECK(isnan(g_fc.p[0].kw.v[48]));          /* nothing yet: unknown, not 0 */
    CHECK(isnan(g_fc.p[0].kwh_today));

    reset();
    f_http_err = ESP_FAIL; f_http_status = 400;
    f_http_body = "{\"error\":true,\"reason\":\"Latitude must be in range of -90 to 90°.\"}";
    tick(t);
    CHECK(strstr(g_fc.err, "Latitude must be") != NULL);
    CHECK(strlen(g_fc.err) < sizeof(g_fc.err));

    /* A long reason is cut, but never inside a UTF-8 character. */
    char cut[8];
    utf8_copy(cut, sizeof(cut), "abcdeü");     /* 7 bytes: fits */
    CHECK(strcmp(cut, "abcdeü") == 0);
    utf8_copy(cut, sizeof(cut), "abcdefü");    /* ü would be split */
    CHECK(strcmp(cut, "abcdef") == 0);
    utf8_copy(cut, sizeof(cut), "abcd€x");     /* € is 3 bytes: fits exactly */
    CHECK(strcmp(cut, "abcd€") == 0);
    utf8_copy(cut, sizeof(cut), "abcde€");     /* 2 of 3 bytes would fit */
    CHECK(strcmp(cut, "abcde") == 0);

    reset();
    f_http_err = ESP_ERR_INVALID_SIZE; f_http_status = 200;
    tick(t);
    CHECK(strstr(g_fc.err, "zu groß") != NULL);

    reset();
    f_http_status = 200; f_http_body = "{\"oops\":1}";
    tick(t);
    CHECK(strstr(g_fc.err, "unlesbar") != NULL);
    CHECK_I(s_backoff, 1);

    /* A failed fetch keeps the weather of the last good one, for every plane. */
    reset();
    f_cfg.nplanes = 2;
    f_cfg.plane[1] = plane("", 2.0f, 30, 180, EM_METER_NONE);
    tick(t);
    CHECK_F(g_fc.p[1].kw.v[48], 2.0 * PR, 1e-3);
    f_fail_call = 2;
    pv_forecast_request();
    tick(t + 10);
    CHECK(strstr(g_fc.err, "HTTP 503") != NULL);
    CHECK_F(g_fc.p[0].kw.v[48], KW5, 1e-3);
    CHECK_F(g_fc.p[1].kw.v[48], 2.0 * PR, 1e-3);
    CHECK_I(g_fc.fetched, t);                  /* the last good fetch */
    CHECK(strcmp(g_fc.p[1].name, "") == 0);
    CHECK_I(f_allocs, f_frees);
}

static void test_nothing_without_location_planes_or_clock(void)
{
    int64_t t = loc(2026, 6, 15, 9, 0);
    reset();
    f_cfg.lat = f_cfg.lon = 0.0f;
    tick(t);
    tick(t + 7200);
    pv_forecast_request();
    tick(t + 7210);
    CHECK_I(f_calls, 0);
    CHECK(strstr(g_fc.err, "Standort") != NULL);
    /* Location entered: the next tick fetches (nothing was ever fetched) and
     * the hint is gone. */
    f_cfg.lat = 48.8f; f_cfg.lon = 9.2f;
    tick(t + 7220);
    CHECK_I(f_calls, 1);
    CHECK(g_fc.err[0] == '\0');

    /* Only one of the two at 0 is a valid place (the equator, Greenwich). */
    reset();
    f_cfg.lat = 0.0f;
    tick(t);
    CHECK_I(f_calls, 1);

    reset();
    f_cfg.nplanes = 0;
    tick(t);
    CHECK_I(f_calls, 0);
    CHECK_I(g_fc.n, 0);
    CHECK(strstr(g_fc.err, "PV-Felder") != NULL);
    CHECK(isnan(g_fc.total_kw.v[0]));

    reset();
    tick(1000);                         /* clock not set yet */
    tick(86400);
    CHECK_I(f_calls, 0);
    CHECK(strstr(g_fc.err, "Uhrzeit") != NULL);
    CHECK_I(s_last_tick, 0);            /* nothing integrates across the jump */
    tick(t);
    CHECK_I(f_calls, 1);
    CHECK(g_fc.err[0] == '\0');
}

static void test_a_gap_is_not_integrated(void)
{
    reset();
    int64_t t = loc(2026, 6, 15, 12, 0);       /* in the sun: 4.5 kW */
    f_dev[0].connected = true;
    f_dev[0].pv_w = 4000.0f;
    tick(t);
    tick(t + 10);
    CHECK_F(g_fc.p[0].meas_kwh_today, 4.0 * 10 / 3600.0, 1e-5);
    tick(t + 10 + 600);                        /* blocked for 10 minutes */
    CHECK_F(g_fc.p[0].meas_kwh_today, 4.0 * 10 / 3600.0, 1e-5);
    tick(t + 620);
    CHECK_F(g_fc.p[0].meas_kwh_today, 4.0 * 20 / 3600.0, 1e-5);
    /* An inverter's night draw is not negative production. */
    f_dev[0].pv_w = -500.0f;
    tick(t + 630);
    CHECK_F(g_fc.p[0].meas_kwh_today, 4.0 * 20 / 3600.0, 1e-5);
}

/* The facade: 8.5 kWp behind a 5 kW hybrid inverter. The forecast never
 * promises more than the inverter can deliver, and clipped hours do not
 * teach the factor that the plane underperforms. */
static void test_ac_limit(void)
{
    reset();
    f_cfg.plane[0] = plane("Süd", 8.5f, 90, 200, 0);
    f_cfg.plane[0].ac_w = 5000;
    int64_t d = loc(2026, 6, 15, 0, 0);
    tick(d + 9 * 3600);
    CHECK(8.5 * PR > 5.0);                          /* would clip */
    CHECK_F(g_fc.p[0].kw.v[48], 5.0, 1e-4);
    CHECK_F(g_fc.total_kw.v[48], 5.0, 1e-4);
    CHECK_F(g_fc.p[0].kwh_today, 10.0, 1e-2);       /* 2 h at the limit */

    /* below the limit nothing changes */
    reset();
    f_cfg.plane[0] = plane("Süd", 8.5f, 90, 200, 0);
    f_cfg.plane[0].ac_w = 5000;
    f_gti = 500.0f;
    tick(d + 9 * 3600);
    CHECK_F(g_fc.p[0].kw.v[48], 8.5 * 0.5 * PR, 1e-4);
    CHECK(g_fc.p[0].kw.v[48] < 5.0f);

    /* a meter that delivers exactly the limit: factor stays 1 */
    reset();
    f_cfg.plane[0] = plane("Süd", 8.5f, 90, 200, 0);
    f_cfg.plane[0].ac_w = 5000;
    run_day(d, 1.0f, 0.0f, 0.0f, -1);
    tick(next_midnight(d) + 15);
    CHECK_F(g_fc.p[0].cal, 1.0, 1e-3);

    /* a factor above 1 does not lift the forecast past the limit */
    reset();
    f_cfg.plane[0] = plane("Süd", 8.5f, 90, 200, 0);
    f_cfg.plane[0].ac_w = 5000;
    run_day(d, 1.3f, 0.0f, 0.0f, -1);
    tick(next_midnight(d) + 15);
    CHECK(g_fc.p[0].cal > 1.0f);
    CHECK_F(g_fc.p[0].kw.v[48], 5.0, 1e-4);

    /* 0 = no limit */
    reset();
    f_cfg.plane[0] = plane("Süd", 8.5f, 90, 200, 0);
    tick(d + 9 * 3600);
    CHECK_F(g_fc.p[0].kw.v[48], 8.5 * PR, 1e-3);
}

/* Ticks over [from, to) with meter 0 reading fac x its forecast. */
static void run_span(int64_t from, int64_t to, float fac)
{
    for (int64_t t = from + STEP / 2; t < to; t += STEP) {
        int64_t tm = t - STEP / 2;
        f_dev[0].connected = true;
        f_dev[0].pv_w      = fac * group_fc(0, tm) * 1000.0f;
        tick(t);
    }
}

static double meas_sum(int j)
{
    double e = 0.0;
    for (int k = 0; k < PV_MEAS_SLOTS; k++) if (isfinite(g_fc.p[j].meas_kwh[k])) e += g_fc.p[j].meas_kwh[k];
    return e;
}

/* The measured curve per quarter hour: what the chart lays over the forecast. */
static void test_measured_slots(void)
{
    reset();
    int64_t d = loc(2026, 6, 15, 0, 0);
    run_day(d, 0.8f, 0.0f, 0.0f, -1);
    CHECK_I(g_fc.meas_t0, d);
    CHECK_I(g_fc.meas_n, 96);
    CHECK_F(g_fc.p[0].meas_kwh[48], 0.8 * KW5 * 0.25, 0.01);   /* in the sun */
    CHECK_I(g_fc.p[0].meas_cov[48], 100);
    CHECK_F(g_fc.p[0].meas_kwh[20], 0.0, 1e-6);                /* night: measured 0 */
    CHECK_I(g_fc.p[0].meas_cov[20], 100);
    CHECK_F(meas_sum(0), g_fc.p[0].meas_kwh_today, 1e-3);
    CHECK(isnan(g_fc.p[0].meas_kwh[96]));                      /* not a slot of this day */

    /* the future and an unmeasured stretch are unknown, not 0 */
    reset();
    tick(d + 9 * 3600);                                        /* forecast in */
    run_span(d + 10 * 3600, d + 11 * 3600, 1.0f);
    CHECK(isnan(g_fc.p[0].meas_kwh[30]));                      /* before: display not running */
    CHECK_I(g_fc.p[0].meas_cov[30], 0);
    CHECK_I(g_fc.p[0].meas_cov[41], 100);
    CHECK(isnan(g_fc.p[0].meas_kwh[60]));                      /* later today */

    /* the inverter asleep at night: 0, measured; offline by day: a gap */
    reset();
    tick(d + 9 * 3600);
    for (int64_t t = d + 9 * 3600 + 15; t < d + 13 * 3600; t += STEP) {
        f_dev[0].connected = false;
        tick(t);
    }
    CHECK_F(g_fc.p[0].meas_kwh[40], 0.0, 1e-9);                /* 10:00 local: night (sun 12-14) */
    CHECK_I(g_fc.p[0].meas_cov[40], 100);
    CHECK(isnan(g_fc.p[0].meas_kwh[49]));                      /* 12:15: sun, no reading */
    CHECK_I(g_fc.p[0].meas_cov[49], 0);
}

/* A software restart (OTA) keeps today's curve; a power cut, another day,
 * a damaged copy or another meter do not. */
static void test_measured_survives_restart(void)
{
    reset();
    int64_t d = loc(2026, 6, 15, 0, 0);
    run_span(d, d + 13 * 3600, 1.0f);                          /* the morning, with the sun */
    double before = meas_sum(0);
    CHECK(before > 1.0);
    float slot48 = g_fc.p[0].meas_kwh[48];

    restart();
    tick(d + 13 * 3600 + 60);
    CHECK_F(g_fc.p[0].meas_kwh[48], slot48, 1e-6);
    CHECK_F(g_fc.p[0].meas_kwh_today, before, 1e-3);           /* the day's sum too */
    run_span(d + 13 * 3600 + 60, d + 14 * 3600, 1.0f);
    CHECK_F(g_fc.p[0].meas_kwh[48], slot48, 1e-6);

    /* damaged: started over */
    restart();
    ((uint8_t *)s_keep.kwh)[5] ^= 0x40;
    tick(d + 14 * 3600 + 60);
    CHECK(isnan(g_fc.p[0].meas_kwh[48]));
    CHECK_F(g_fc.p[0].meas_kwh_today, 0.0, 1e-9);

    /* yesterday's copy does not become today's */
    reset();
    run_span(d, d + 13 * 3600, 1.0f);
    restart();
    tick(next_midnight(d) + 13 * 3600);
    CHECK_I(g_fc.meas_t0, next_midnight(d));
    CHECK(isnan(g_fc.p[0].meas_kwh[48]));

    /* another meter for the plane: its curve starts over */
    reset();
    run_span(d, d + 13 * 3600, 1.0f);
    restart();
    f_cfg.plane[0].meter = 1;
    tick(d + 13 * 3600 + 60);
    CHECK(isnan(g_fc.p[0].meas_kwh[48]));

    /* a changed geometry keeps it: same panels, same energy */
    reset();
    run_span(d, d + 13 * 3600, 1.0f);
    slot48 = g_fc.p[0].meas_kwh[48];
    restart();
    f_cfg.plane[0].azimuth = 200;
    tick(d + 13 * 3600 + 60);
    CHECK_F(g_fc.p[0].meas_kwh[48], slot48, 1e-6);

    /* power cut */
    reset();
    tick(d + 13 * 3600 + 60);
    CHECK(isnan(g_fc.p[0].meas_kwh[48]));
}

int main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    RUN(test_the_model);
    RUN(test_the_request);
    RUN(test_parsing_nulls_and_the_slot_shift);
    RUN(test_series_and_daily_sums);
    RUN(test_dst_days);
    RUN(test_calibration_one_day);
    RUN(test_calibration_is_an_ema);
    RUN(test_calibration_clamps);
    RUN(test_coverage_and_size_rules);
    RUN(test_shared_meter_groups);
    RUN(test_geometry_change_resets_and_restart_keeps);
    RUN(test_schedule_and_backoff);
    RUN(test_error_texts);
    RUN(test_nothing_without_location_planes_or_clock);
    RUN(test_a_gap_is_not_integrated);
    RUN(test_ac_limit);
    RUN(test_measured_slots);
    RUN(test_measured_survives_restart);
    return t_report("pv_forecast");
}

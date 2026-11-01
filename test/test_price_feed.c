#include "harness.h"
#include "fakes.h"

#include <stdlib.h>
#include <time.h>

/* The price feed decides on its own when to call out to Tibber and
 * Energy-Charts, and the planner trusts what comes back. What matters here:
 * the numbers land in the right quarter hour (offsets in the timestamps,
 * hourly answers, the 100-slot DST day), errors become a text instead of
 * garbage prices, and the schedule leaves rate-limited APIs alone. */

/* No heap_caps on the host. */
#define PF_BUF_ALLOC(n) malloc(n)
#define PF_BUF_FREE(p)  free(p)
#define PF_BIG_ALLOC(n) calloc(1, (n))
#include "price_feed.c"

/* ----------------------------------------------------------- fakes
 * https_get answers from a queue of canned responses; an empty queue is
 * "no connection" (status 0), like the real one without network; status -1
 * is "postponed for lack of internal RAM". Like the
 * real one it hands over the body for a non-200 status as well. */
typedef struct {
    int         status;
    const char *body;
} fresp_t;
static fresp_t f_q[16];
static int     f_qn, f_qi, f_calls;
static char    f_url[256], f_auth[128], f_post[512];

static void f_push(int status, const char *body)
{
    f_q[f_qn].status = status;
    f_q[f_qn].body   = body;
    f_qn++;
}

esp_err_t https_get(const https_req_t *rq, char *buf, size_t cap, size_t *len, int *status)
{
    f_calls++;
    snprintf(f_url, sizeof(f_url), "%s", rq->url);
    snprintf(f_auth, sizeof(f_auth), "%s", rq->auth_bearer ? rq->auth_bearer : "");
    snprintf(f_post, sizeof(f_post), "%s", rq->post_json ? rq->post_json : "");
    *len = 0; *status = 0; buf[0] = '\0';
    if (f_qi >= f_qn) return ESP_FAIL;
    fresp_t r = f_q[f_qi++];
    if (r.status < 0) return ESP_ERR_NO_MEM;    /* postponed: RAM too low */
    size_t  n = strlen(r.body);
    *status = r.status;
    if (n >= cap) {
        memcpy(buf, r.body, cap - 1);
        buf[cap - 1] = '\0';
        *len = cap - 1;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, r.body, n + 1);
    *len = n;
    return r.status == 200 ? ESP_OK : ESP_FAIL;
}

static em_cfg_t f_cfg;
void em_cfg_get(em_cfg_t *out) { *out = f_cfg; }

/* ----------------------------------------------------------- helpers */
static int64_t lt(int y, int mo, int d, int h, int mi)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
                     .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return (int64_t)mktime(&tm);
}

/* ISO-8601 with the local offset, the way Tibber writes it. */
static void iso(int64_t t, char *out, size_t cap)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    long off = tm.tm_gmtoff;
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.000%c%02ld:%02ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             off < 0 ? '-' : '+', labs(off) / 3600, (labs(off) % 3600) / 60);
}

/* today: n0 entries from t0, tomorrow: n1 entries from t1, `step` seconds
 * apart. Prices today 0.25 + i/1000 (energy 0.10 + i/1000) EUR/kWh,
 * tomorrow 0.30 + i/1000 (0.15 + i/1000). */
static char *tibber_body(char *o, size_t cap, int64_t t0, int n0, int64_t t1, int n1,
                         int step, bool null_home_first)
{
    size_t p = 0;
    p += snprintf(o + p, cap - p, "{\"data\":{\"viewer\":{\"homes\":[%s"
                  "{\"id\":\"b\",\"currentSubscription\":{\"priceInfo\":{\"today\":[",
                  null_home_first ? "{\"id\":\"a\",\"currentSubscription\":null}," : "");
    char ts[40];
    for (int i = 0; i < n0; i++) {
        iso(t0 + (int64_t)i * step, ts, sizeof(ts));
        p += snprintf(o + p, cap - p, "%s{\"total\":%.4f,\"energy\":%.4f,\"startsAt\":\"%s\"}",
                      i ? "," : "", 0.25 + i / 1000.0, 0.10 + i / 1000.0, ts);
    }
    p += snprintf(o + p, cap - p, "],\"tomorrow\":[");
    for (int i = 0; i < n1; i++) {
        iso(t1 + (int64_t)i * step, ts, sizeof(ts));
        p += snprintf(o + p, cap - p, "%s{\"total\":%.4f,\"energy\":%.4f,\"startsAt\":\"%s\"}",
                      i ? "," : "", 0.30 + i / 1000.0, 0.15 + i / 1000.0, ts);
    }
    snprintf(o + p, cap - p, "]}}}]}}}");
    return o;
}

/* Energy-Charts /price: every 7th value null. */
static char *ec_body(char *o, size_t cap, int64_t t0, int n, int step)
{
    size_t p = snprintf(o, cap, "{\"license_info\":\"CC BY 4.0\",\"unix_seconds\":[");
    for (int i = 0; i < n; i++)
        p += snprintf(o + p, cap - p, "%s%lld", i ? "," : "", (long long)(t0 + (int64_t)i * step));
    p += snprintf(o + p, cap - p, "],\"price\":[");
    for (int i = 0; i < n; i++) {
        if (i % 7 == 3) p += snprintf(o + p, cap - p, "%snull", i ? "," : "");
        else            p += snprintf(o + p, cap - p, "%s%.2f", i ? "," : "", 100.0 + i);
    }
    snprintf(o + p, cap - p, "],\"unit\":\"EUR / MWh\",\"deprecated\":false}");
    return o;
}

/* ren_share_forecast: ren 40 + i/10 %, solar i/10, wind on 30, wind off 5;
 * ren is null at slot 10. */
static char *mix_body(char *o, size_t cap, int64_t t0, int n)
{
    static const char *keys[4] = { "ren_share", "solar_share", "wind_onshore_share",
                                   "wind_offshore_share" };
    size_t p = snprintf(o, cap, "{\"unix_seconds\":[");
    for (int i = 0; i < n; i++)
        p += snprintf(o + p, cap - p, "%s%lld", i ? "," : "", (long long)(t0 + (int64_t)i * 900));
    p += snprintf(o + p, cap - p, "]");
    for (int k = 0; k < 4; k++) {
        p += snprintf(o + p, cap - p, ",\"%s\":[", keys[k]);
        for (int i = 0; i < n; i++) {
            double v = k == 0 ? 40 + i / 10.0 : k == 1 ? i / 10.0 : k == 2 ? 30 : 5;
            if (k == 0 && i == 10) p += snprintf(o + p, cap - p, "%snull", i ? "," : "");
            else                   p += snprintf(o + p, cap - p, "%s%.1f", i ? "," : "", v);
        }
        p += snprintf(o + p, cap - p, "]");
    }
    snprintf(o + p, cap - p, ",\"substitute\":false}");
    return o;
}

static char b1[64 * 1024], b2[64 * 1024], b3[64 * 1024];
static price_set_t ps;
static grid_mix_t  gm;

static void reset(bool token)
{
    s_gen_on = false;           /* the generation forecast has its own tests */
    s_gen_stage = 0;
    memset(&s_genj, 0, sizeof(s_genj));
    s_ec_last = 0;
    if (!s_big) s_big = PF_BIG_ALLOC(sizeof(*s_big));
    memset(s_big, 0, sizeof(*s_big));
    memset(&s_tib, 0, sizeof(s_tib));
    memset(&s_ec, 0, sizeof(s_ec));
    memset(&s_mixj, 0, sizeof(s_mixj));
    s_offset         = PF_OFFSET_DEF;
    s_offset_learned = false;
    s_force_px = s_force_mix = false;
    f_qn = f_qi = f_calls = 0;
    f_url[0] = '\0';
    memset(&f_cfg, 0, sizeof(f_cfg));
    if (token) snprintf(f_cfg.tibber_token, sizeof(f_cfg.tibber_token), "tok123");
}

static bool url_has(const char *s) { return strstr(f_url, s) != NULL; }

/* ----------------------------------------------------------- tests */
/* Before the first tick there is no state block yet: the getters must hand
 * out an empty set, not dereference NULL. */
static void test_before_first_tick(void)
{
    memset(&ps, 0xA5, sizeof(ps));
    memset(&gm, 0xA5, sizeof(gm));
    CHECK(s_big == NULL);
    price_feed_get(&ps);
    price_feed_get_mix(&gm);
    CHECK_I(ps.total.n, 0);
    CHECK_I(ps.fetched, 0);
    CHECK(ps.src[0] == '\0' && ps.err[0] == '\0');
    CHECK_I(gm.ren.n, 0);
    price_feed_tick(1600000000);                    /* no clock: still nothing */
    CHECK(s_big == NULL);
}

static void test_iso(void)
{
    int64_t m = lt(2026, 10, 21, 0, 0), t;
    CHECK(iso_to_unix("2026-10-21T00:00:00.000+02:00", &t)); CHECK_I(t, m);
    CHECK(iso_to_unix("2026-10-20T22:00:00Z", &t));          CHECK_I(t, m);
    CHECK(iso_to_unix("2026-10-21T00:00:00+0200", &t));      CHECK_I(t, m);
    CHECK(iso_to_unix("2026-10-20T23:00:00.000+01:00", &t)); CHECK_I(t, m);
    CHECK(iso_to_unix("2026-10-20T17:30:00-04:30", &t));     CHECK_I(t, m);
    CHECK(!iso_to_unix("2026-10-21T00:00:00", &t));          /* no offset: refuse, not guess */
    CHECK(!iso_to_unix("2026-10-21", &t));
    CHECK(!iso_to_unix("2026-13-21T00:00:00Z", &t));
    CHECK(!iso_to_unix(NULL, &t));
}

static void test_tibber_today(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 96, 0, 0, 900, true));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    CHECK_I(f_calls, 1);
    CHECK(url_has("api.tibber.com/v1-beta/gql"));
    CHECK(strcmp(f_auth, "tok123") == 0);
    CHECK(strstr(f_post, "QUARTER_HOURLY") != NULL);
    /* the body must be valid JSON */
    cJSON *q = cJSON_Parse(f_post);
    CHECK(cJSON_IsString(cJSON_GetObjectItem(q, "query")));
    cJSON_Delete(q);

    price_feed_get(&ps);
    CHECK(strcmp(ps.src, "tibber") == 0);
    CHECK(ps.err[0] == '\0');
    CHECK(!ps.tomorrow);
    CHECK_I(ps.fetched, lt(2026, 10, 21, 10, 0));
    CHECK_I(ps.total.t0, t0);
    CHECK_I(ps.total.n, 96);
    CHECK_I(ps.energy.n, 96);
    CHECK_F(ps.total.v[0], 25.0, 1e-3);           /* EUR -> ct */
    CHECK_F(ps.energy.v[0], 10.0, 1e-3);
    CHECK_F(ps.total.v[95], 34.5, 1e-3);
    CHECK_F(em_series_at(&ps.total, lt(2026, 10, 21, 12, 0)), 25.0 + 4.8, 1e-3);
    CHECK(isnan(ps.total.v[96]));

    /* fees learned: mean(total - energy * 1.19) = 13.1 - 0.019 * 47.5 */
    bool learned = false;
    CHECK_F(price_feed_offset_ct(&learned), 13.1 - 0.019 * 47.5, 1e-3);
    CHECK(learned);
}

static void test_tibber_tomorrow(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0), t1 = lt(2026, 10, 22, 0, 0);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 96, t1, 96, 900, false));
    price_feed_tick(lt(2026, 10, 21, 14, 0));
    price_feed_get(&ps);
    CHECK(ps.tomorrow);
    CHECK_I(ps.total.n, 192);
    CHECK_F(ps.total.v[96], 30.0, 1e-3);
    CHECK_F(ps.energy.v[191], 15.0 + 9.5, 1e-3);
    CHECK_I(s_tib.day, t0);
    CHECK(s_tib.tomorrow);
}

/* Entries before today's midnight (an answer from just before the day
 * flipped) are dropped, not written in front of slot 0. */
static void test_tibber_before_t0(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    f_push(200, tibber_body(b1, sizeof(b1), t0 - 3600, 100, 0, 0, 900, false));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    price_feed_get(&ps);
    CHECK(ps.err[0] == '\0');
    CHECK_I(ps.total.t0, t0);
    CHECK_I(ps.total.n, 96);
    CHECK_F(ps.total.v[0], 25.4, 1e-3);
    CHECK_F(ps.total.v[95], 25.0 + 9.9, 1e-3);
    CHECK_I(ps.energy.t0, t0);
    CHECK_I(ps.energy.n, 96);
}

/* Tibber answering hourly despite QUARTER_HOURLY: each value fills its hour. */
static void test_tibber_hourly(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 24, 0, 0, 3600, false));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    price_feed_get(&ps);
    CHECK_I(ps.total.n, 96);
    for (int i = 0; i < 4; i++) CHECK_F(ps.total.v[i], 25.0, 1e-3);
    CHECK_F(ps.total.v[4], 25.1, 1e-3);
    CHECK_F(ps.total.v[95], 25.0 + 2.3, 1e-3);     /* the last hour too */
    int nan = 0;
    for (int i = 0; i < 96; i++) nan += isnan(ps.total.v[i]);
    CHECK_I(nan, 0);
}

static void test_tibber_errors(void)
{
    reset(true);
    int64_t now = lt(2026, 10, 21, 10, 0);
    f_push(200, "{\"errors\":[{\"message\":\"Context creation failed: invalid token\","
                "\"extensions\":{\"code\":\"UNAUTHENTICATED\"}}],\"data\":null}");
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: Token ungültig") == 0);
    CHECK(ps.src[0] == '\0');
    CHECK_I(ps.total.n, 0);
    CHECK_I(s_tib.fails, 1);

    /* 401 with an empty body */
    reset(true);
    f_push(401, "");
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: Token ungültig") == 0);

    /* some other GraphQL error: its message, tidied */
    reset(true);
    f_push(200, "{\"errors\":[{\"message\":\"internal \\\"oops\\\"\"}],\"data\":null}");
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strncmp(ps.err, "Tibber: internal", 16) == 0);
    CHECK(strchr(ps.err, '"') == NULL);

    /* a failure keeps the prices that are there */
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 96, 0, 0, 900, false));
    price_feed_tick(now);
    f_push(500, "oops");
    s_force_px = true;
    price_feed_tick(now + 600);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: HTTP 500") == 0);
    CHECK_I(ps.total.n, 96);
    CHECK(strcmp(ps.src, "tibber") == 0);

    /* no home with a contract */
    reset(true);
    f_push(200, "{\"data\":{\"viewer\":{\"homes\":[{\"id\":\"a\",\"currentSubscription\":null}]}}}");
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: kein Vertrag mit Preisen gefunden") == 0);

    /* today empty (Tibber has not flipped its day yet) */
    reset(true);
    f_push(200, tibber_body(b1, sizeof(b1), lt(2026, 10, 20, 0, 0), 96, 0, 0, 900, false));
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: keine Preise für heute") == 0);

    /* no network */
    reset(true);
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: keine Verbindung") == 0);

    /* postponed for lack of RAM: said as such, not as a dead link */
    reset(true);
    f_push(-1, "");
    price_feed_tick(now);
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: verschoben, Speicher gerade knapp") == 0);
}

static void test_energy_charts(void)
{
    reset(false);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    f_push(200, ec_body(b1, sizeof(b1), t0, 192, 900));
    price_feed_tick(lt(2026, 10, 21, 14, 0));
    CHECK_I(f_calls, 1);
    CHECK(url_has("api.energy-charts.info/price?bzn=DE-LU&start=2026-10-21&end=2026-10-22"));
    CHECK(f_auth[0] == '\0' && f_post[0] == '\0');
    price_feed_get(&ps);
    CHECK(strcmp(ps.src, "energy-charts") == 0);
    CHECK(ps.err[0] == '\0');
    CHECK(ps.tomorrow);
    CHECK_I(ps.total.n, 192);
    CHECK_F(ps.energy.v[0], 10.0, 1e-3);                 /* 100 EUR/MWh = 10 ct/kWh */
    CHECK_F(ps.total.v[0], 10.0 * 1.19 + 20.0, 1e-3);    /* default fees */
    CHECK(isnan(ps.energy.v[3]) && isnan(ps.total.v[3])); /* null stays unknown */
    CHECK_F(ps.energy.v[100], 20.0, 1e-3);

    /* fees learned from Tibber carry over to the fallback */
    reset(true);
    f_push(200, tibber_body(b2, sizeof(b2), t0, 96, 0, 0, 900, false));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    float off = price_feed_offset_ct(NULL);
    f_cfg.tibber_token[0] = '\0';                        /* token removed */
    f_push(200, b1);
    price_feed_tick(lt(2026, 10, 21, 10, 1));
    CHECK(url_has("energy-charts.info/price"));
    price_feed_get(&ps);
    CHECK(strcmp(ps.src, "energy-charts") == 0);
    CHECK_F(ps.total.v[1], 10.1 * 1.19 + off, 1e-3);

    /* hourly Energy-Charts data is spread like Tibber's */
    reset(false);
    f_push(200, ec_body(b1, sizeof(b1), t0, 24, 3600));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    price_feed_get(&ps);
    CHECK_I(ps.total.n, 96);
    CHECK_F(ps.energy.v[7], 10.1, 1e-3);
    CHECK(isnan(ps.energy.v[12]) && isnan(ps.energy.v[15]));   /* hour 3 was null */

    /* garbage */
    reset(false);
    f_push(200, "{\"unix_seconds\":[1,2,\"x\"],\"price\":[1,2,3]}");
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Energy-Charts: unerwartete Antwort") == 0);
    CHECK_I(ps.total.n, 0);
}

/* Tibber fails -> next tick Energy-Charts stands in, Tibber's error stays
 * visible because the user can fix it. */
static void test_fallback(void)
{
    reset(true);
    int64_t now = lt(2026, 10, 21, 10, 0), t0 = lt(2026, 10, 21, 0, 0);
    f_push(429, "");
    f_push(200, ec_body(b1, sizeof(b1), t0, 96, 900));
    price_feed_tick(now);
    CHECK(url_has("tibber"));
    price_feed_tick(now + 10);
    CHECK(url_has("energy-charts.info/price"));
    price_feed_get(&ps);
    CHECK(strcmp(ps.src, "energy-charts") == 0);
    CHECK(strcmp(ps.err, "Tibber: zu viele Anfragen (HTTP 429)") == 0);
    CHECK_I(ps.total.n, 96);

    /* then the mix -- but not before PF_EC_SPACING after the last
     * Energy-Charts request -- then quiet: Tibber holds off 60 min after a 429 */
    f_push(200, mix_body(b2, sizeof(b2), t0, 96));
    int before = f_calls;
    price_feed_tick(now + 20);
    CHECK_I(f_calls, before);                         /* too close to the last EC call */
    price_feed_tick(now + 10 + PF_EC_SPACING);
    CHECK(url_has("ren_share_forecast"));
    int calls = f_calls;
    for (int64_t t = now + 20 + PF_EC_SPACING; t < now + 3600; t += 10) price_feed_tick(t);
    CHECK_I(f_calls, calls);

    /* at 60 min Tibber again, and it works: Tibber replaces the estimate */
    f_push(200, tibber_body(b3, sizeof(b3), t0, 96, 0, 0, 900, false));
    price_feed_tick(now + 3600);
    CHECK_I(f_calls, calls + 1);
    CHECK(url_has("tibber"));
    price_feed_get(&ps);
    CHECK(strcmp(ps.src, "tibber") == 0);
    CHECK(ps.err[0] == '\0');
}

/* With Tibber's real prices for today at hand, a failing tomorrow-retry
 * must not let the Energy-Charts estimate overwrite them. */
static void test_no_fallback_over_tibber(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    s_mixj.day = t0; s_mixj.last_ok = s_mixj.last_try = lt(2026, 10, 21, 23, 0);  /* mix quiet */
    f_push(200, tibber_body(b1, sizeof(b1), t0, 96, 0, 0, 900, false));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    f_push(503, "");
    f_push(200, ec_body(b2, sizeof(b2), t0, 192, 900));
    price_feed_tick(lt(2026, 10, 21, 13, 0));
    CHECK(url_has("tibber"));
    int c = f_calls;
    for (int64_t t = lt(2026, 10, 21, 13, 0) + 10; t < lt(2026, 10, 21, 13, 5); t += 10)
        price_feed_tick(t);
    CHECK_I(f_calls, c);
    price_feed_get(&ps);
    CHECK(strcmp(ps.src, "tibber") == 0);
    CHECK(strcmp(ps.err, "Tibber: HTTP 503") == 0);
}

static void test_mix(void)
{
    reset(false);
    int64_t t0 = lt(2026, 10, 21, 0, 0);
    s_ec.day = t0; s_ec.tomorrow = true;                                  /* prices not due */
    s_ec.last_try = s_ec.last_ok = lt(2026, 10, 21, 9, 0);
    f_push(200, mix_body(b1, sizeof(b1), t0, 96));
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    CHECK_I(f_calls, 1);
    CHECK(url_has("api.energy-charts.info/ren_share_forecast?country=de"));
    price_feed_get_mix(&gm);
    CHECK(gm.err[0] == '\0');
    CHECK_I(gm.fetched, lt(2026, 10, 21, 10, 0));
    CHECK_I(gm.ren.t0, t0);
    CHECK_I(gm.ren.n, 96);
    CHECK_I(gm.wind_off.n, 96);
    CHECK_F(gm.ren.v[0], 40.0, 1e-3);
    CHECK_F(gm.ren.v[95], 49.5, 1e-3);
    CHECK(isnan(gm.ren.v[10]));
    CHECK_F(gm.solar.v[50], 5.0, 1e-3);
    CHECK_F(gm.wind_on.v[3], 30.0, 1e-3);
    CHECK_F(gm.wind_off.v[3], 5.0, 1e-3);

    /* 429: error, data kept, backoff at least 60 min */
    f_push(429, "Too Many Requests");
    price_feed_tick(lt(2026, 10, 21, 13, 0));
    price_feed_get_mix(&gm);
    CHECK(strcmp(gm.err, "Energy-Charts: zu viele Anfragen (HTTP 429)") == 0);
    CHECK_I(gm.ren.n, 96);
    CHECK_I(mix_due(&s_mixj, lt(2026, 10, 21, 13, 0)), lt(2026, 10, 21, 14, 0));
}

static void test_schedule_price(void)
{
    pf_job_t j = { 0 };
    int64_t d0 = lt(2026, 10, 21, 0, 0);

    /* nothing for today: now */
    CHECK_I(price_due(&j, lt(2026, 10, 21, 10, 0)), lt(2026, 10, 21, 10, 0));

    /* fetched at 10:00 without tomorrow: 13:00 beats the 6 h refresh */
    j.day = d0; j.last_try = j.last_ok = lt(2026, 10, 21, 10, 0);
    CHECK_I(price_due(&j, lt(2026, 10, 21, 10, 1)), lt(2026, 10, 21, 13, 0));
    /* fetched at 06:05: refresh at 12:05 comes first */
    j.last_try = j.last_ok = lt(2026, 10, 21, 6, 5);
    CHECK_I(price_due(&j, lt(2026, 10, 21, 7, 0)), lt(2026, 10, 21, 12, 5));
    /* from 13:00 every 30 min */
    j.last_try = j.last_ok = lt(2026, 10, 21, 13, 0);
    CHECK_I(price_due(&j, lt(2026, 10, 21, 13, 1)), lt(2026, 10, 21, 13, 30));
    j.last_try = j.last_ok = lt(2026, 10, 21, 20, 30);
    CHECK_I(price_due(&j, lt(2026, 10, 21, 20, 31)), lt(2026, 10, 22, 2, 30));  /* not after 21:00 */
    /* tomorrow is in: 6 h */
    j.tomorrow = true; j.last_try = j.last_ok = lt(2026, 10, 21, 13, 30);
    CHECK_I(price_due(&j, lt(2026, 10, 21, 13, 31)), lt(2026, 10, 21, 19, 30));

    /* day roll at 00:05, not at midnight */
    j.last_try = j.last_ok = lt(2026, 10, 21, 22, 0);
    CHECK(price_due(&j, lt(2026, 10, 22, 0, 4)) > lt(2026, 10, 22, 0, 4));
    CHECK_I(price_due(&j, lt(2026, 10, 22, 0, 5)), lt(2026, 10, 22, 0, 5));

    /* backoff 5, 10, 20, 60, 60 min */
    pf_job_t f = { 0 };
    f.last_try = lt(2026, 10, 21, 10, 0);
    int64_t want[5] = { 300, 600, 1200, 3600, 3600 };
    for (int i = 0; i < 5; i++) {
        f.fails = (uint8_t)(i + 1);
        CHECK_I(price_due(&f, f.last_try + 1), f.last_try + want[i]);
    }
    f.fails = 1; f.r429 = true;
    CHECK_I(price_due(&f, f.last_try + 1), f.last_try + 3600);
    CHECK_I(forced_due(&f, f.last_try + 1), f.last_try + 3600);   /* force does not break a 429 hold */
    f.r429 = false;
    CHECK_I(forced_due(&f, f.last_try + 1), f.last_try + 300);
    f.fails = 0;
    CHECK_I(forced_due(&f, f.last_try + 1), f.last_try + 1);

    /* Energy-Charts at most every 2 h, even with nothing for today */
    pf_job_t e = { 0 };
    e.last_try = lt(2026, 10, 21, 10, 0); e.fails = 1;
    CHECK_I(ec_due(&e, lt(2026, 10, 21, 10, 1)), lt(2026, 10, 21, 12, 0));
    e.fails = 0; e.day = d0; e.last_ok = e.last_try;
    CHECK_I(ec_due(&e, lt(2026, 10, 21, 10, 1)), lt(2026, 10, 21, 13, 0));
    e.last_try = e.last_ok = lt(2026, 10, 21, 13, 0);
    CHECK_I(ec_due(&e, lt(2026, 10, 21, 13, 1)), lt(2026, 10, 21, 15, 0));
}

static void test_schedule_mix(void)
{
    pf_job_t j = { 0 };
    CHECK_I(mix_due(&j, lt(2026, 10, 21, 10, 0)), lt(2026, 10, 21, 10, 0));
    j.day = lt(2026, 10, 21, 0, 0); j.last_try = j.last_ok = lt(2026, 10, 21, 10, 0);
    j.tomorrow = true;                                 /* tomorrow in: plain 3 h */
    CHECK_I(mix_due(&j, lt(2026, 10, 21, 10, 1)), lt(2026, 10, 21, 13, 0));
    /* tomorrow missing: from 12:00 every 45 min, until 23:00 */
    j.tomorrow = false;
    CHECK_I(mix_due(&j, lt(2026, 10, 21, 10, 1)), lt(2026, 10, 21, 12, 0));
    j.last_try = j.last_ok = lt(2026, 10, 21, 12, 0);
    CHECK_I(mix_due(&j, lt(2026, 10, 21, 12, 1)), lt(2026, 10, 21, 12, 45));
    j.last_try = j.last_ok = lt(2026, 10, 21, 22, 30);
    CHECK_I(mix_due(&j, lt(2026, 10, 21, 22, 31)), lt(2026, 10, 22, 1, 30));   /* 23:15 is past the window */
    CHECK_I(mix_due(&j, lt(2026, 10, 21, 23, 5)), lt(2026, 10, 22, 1, 30));    /* no chase after 23:00 */
    CHECK_I(mix_due(&j, lt(2026, 10, 22, 0, 10)), lt(2026, 10, 22, 0, 10));    /* the new day */
    j.last_try = j.last_ok = lt(2026, 10, 21, 23, 0);
    CHECK(mix_due(&j, lt(2026, 10, 22, 0, 9)) > lt(2026, 10, 22, 0, 9));
    CHECK_I(mix_due(&j, lt(2026, 10, 22, 0, 10)), lt(2026, 10, 22, 0, 10));
    j.fails = 2; j.last_try = lt(2026, 10, 22, 0, 10);
    CHECK_I(mix_due(&j, lt(2026, 10, 22, 0, 11)), lt(2026, 10, 22, 0, 20));
}

/* Through the tick: a day with Tibber, its 13:00 retries and the roll. */
static void test_schedule_tick(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 21, 0, 0), t1 = lt(2026, 10, 22, 0, 0);
    s_mixj.day = t0; s_mixj.last_ok = s_mixj.last_try = lt(2026, 10, 21, 23, 0);  /* keep the mix quiet */
    tibber_body(b1, sizeof(b1), t0, 96, 0, 0, 900, false);
    tibber_body(b2, sizeof(b2), t0, 96, t1, 96, 900, false);
    for (int i = 0; i < 4; i++) f_push(200, b1);
    f_push(200, b2);
    f_push(200, b2);
    int64_t start = lt(2026, 10, 21, 9, 0);
    int64_t when[8];
    int     nw = 0, last = 0;
    for (int64_t t = start; t < lt(2026, 10, 21, 23, 0); t += 10) {
        price_feed_tick(t);
        if (f_calls != last) { if (nw < 8) when[nw] = t; nw++; last = f_calls; }
    }
    /* 09:00 (nothing yet), 13:00, 13:30, 14:00 (no tomorrow), 14:30 tomorrow in, then 20:30 */
    CHECK_I(nw, 6);
    CHECK_I(when[0], start);
    CHECK_I(when[1], lt(2026, 10, 21, 13, 0));
    CHECK_I(when[2], lt(2026, 10, 21, 13, 30));
    CHECK_I(when[3], lt(2026, 10, 21, 14, 0));
    CHECK_I(when[4], lt(2026, 10, 21, 14, 30));
    CHECK_I(when[5], lt(2026, 10, 21, 20, 30));       /* 6 h refresh */
    price_feed_get(&ps);
    CHECK(ps.tomorrow);
    CHECK_I(ps.total.n, 192);
    CHECK(ps.err[0] == '\0');

    /* forced refresh: right away */
    f_push(200, b2);
    int c = f_calls;
    price_feed_request();
    price_feed_tick(lt(2026, 10, 21, 20, 36));
    CHECK_I(f_calls, c + 1);
    CHECK(url_has("tibber"));
    CHECK(!s_force_px);
    CHECK(s_force_mix);
    price_feed_tick(lt(2026, 10, 21, 20, 37));        /* the mix follows */
    CHECK_I(f_calls, c + 2);
    CHECK(url_has("ren_share"));
    CHECK(!s_force_mix);

    /* day roll: at 00:05 yesterday's tomorrow becomes today, even when the
     * re-fetch fails */
    c = f_calls;
    price_feed_tick(lt(2026, 10, 22, 0, 4));
    CHECK_I(f_calls, c);
    price_feed_get(&ps);
    CHECK_I(ps.total.t0, t0);
    price_feed_tick(lt(2026, 10, 22, 0, 5));
    CHECK_I(f_calls, c + 1);                          /* empty queue: fails */
    price_feed_get(&ps);
    CHECK_I(ps.total.t0, t1);
    CHECK_I(ps.total.n, 96);
    CHECK_I(ps.energy.n, 96);
    CHECK(!ps.tomorrow);
    CHECK_F(ps.total.v[0], 30.0, 1e-3);
    CHECK_F(ps.energy.v[95], 24.5, 1e-3);
    CHECK(isnan(ps.total.v[96]));
}

/* 2026-10-25: clocks go back, the day has 100 quarter hours, today plus
 * tomorrow 196 -- more than the grid holds. */
/* ------------------------------------------------ generation forecast */

static char *gen_body(char *o, size_t cap, int64_t t0, int n, float mw)
{
    int p = snprintf(o, cap, "{\"unix_seconds\":[");
    for (int i = 0; i < n; i++) p += snprintf(o + p, cap - p, "%s%lld", i ? "," : "", (long long)(t0 + 900LL * i));
    p += snprintf(o + p, cap - p, "],\"forecast_values\":[");
    for (int i = 0; i < n; i++)
        p += i == 5 ? snprintf(o + p, cap - p, ",null") : snprintf(o + p, cap - p, "%s%.1f", i ? "," : "", mw + i);
    snprintf(o + p, cap - p, "],\"production_type\":\"x\",\"forecast_type\":\"day-ahead\",\"deprecated\":false}");
    return o;
}

/* Generation in GW over [t0, t0 + n slots): null for i in [nf, nt), the rest
 * mw + i. */
static char *gen_body2(char *o, size_t cap, int64_t t0, int n, float mw, int nf, int nt)
{
    int p = snprintf(o, cap, "{\"unix_seconds\":[");
    for (int i = 0; i < n; i++) p += snprintf(o + p, cap - p, "%s%lld", i ? "," : "", (long long)(t0 + 900LL * i));
    p += snprintf(o + p, cap - p, "],\"forecast_values\":[");
    for (int i = 0; i < n; i++)
        p += (i >= nf && i < nt) ? snprintf(o + p, cap - p, "%snull", i ? "," : "")
                                 : snprintf(o + p, cap - p, "%s%.1f", i ? "," : "", mw + i);
    snprintf(o + p, cap - p, "]}");
    return o;
}

/* Six requests per round -- per type day-ahead, then intraday -- each at
 * least PF_EC_SPACING after the previous Energy-Charts call, MW -> GW, then
 * quiet until the refresh. */
static void test_generation(void)
{
    reset(false);
    s_gen_on = true;
    int64_t now = lt(2026, 10, 21, 10, 0), t0 = lt(2026, 10, 21, 0, 0);
    s_mixj.day = t0; s_mixj.last_ok = now;            /* mix is fresh */
    s_mixj.tomorrow = true;                            /* ... and complete: no chase */
    s_ec.day = t0; s_ec.last_ok = now; s_ec.last_try = now;
    int c0 = f_calls;
    int64_t t = now;
    static char bd[6][16384];
    static const char *const want[6] = {
        "production_type=solar&forecast_type=day-ahead", "production_type=solar&forecast_type=intraday",
        "production_type=wind_onshore&forecast_type=day-ahead", "production_type=wind_onshore&forecast_type=intraday",
        "production_type=wind_offshore&forecast_type=day-ahead", "production_type=wind_offshore&forecast_type=intraday",
    };
    static const float mw[3] = { 500, 10000, 2000 };
    for (int k = 0; k < 6; k++) {
        /* day-ahead: today and tomorrow (a morning); intraday: today only,
         * 100 MW more -- the newer numbers win where both have one */
        f_push(200, k % 2 ? gen_body2(bd[k], sizeof(bd[k]), t0, 192, mw[k / 2] + 100, 96, 192)
                          : gen_body2(bd[k], sizeof(bd[k]), t0, 192, mw[k / 2], 5, 6));
        price_feed_tick(t + k * PF_EC_SPACING);
        CHECK(url_has(want[k]));
        if (k == 0) {
            price_feed_tick(t + 10);
            CHECK_I(f_calls, c0 + 1);                  /* spacing: not yet */
        }
    }
    CHECK_I(f_calls, c0 + 6);

    price_feed_get_mix(&gm);
    CHECK_I(gm.wind_on_gw.t0, t0);
    CHECK_F(gm.wind_on_gw.v[0], 10.1, 1e-4);           /* intraday over day-ahead, MW -> GW */
    CHECK_F(gm.wind_off_gw.v[1], 2.101, 1e-4);
    CHECK_F(gm.solar_gw.v[2], 0.602, 1e-4);
    CHECK_F(gm.wind_on_gw.v[5], 10.105, 1e-4);         /* day-ahead null, intraday has it */
    CHECK_F(gm.wind_on_gw.v[100], 10.1, 1e-4);         /* tomorrow: day-ahead (intraday null) */
    CHECK(gm.gen_fetched > 0);

    for (int64_t k = t + 6 * PF_EC_SPACING; k < t + 3 * 3600 - 600; k += 60) price_feed_tick(k);
    CHECK_I(f_calls, c0 + 6);                          /* quiet until the 3-h refresh */
}

/* The evening of 2026-10-24: tomorrow's day-ahead is out, and day-ahead now
 * answers null for today. Today must not be lost -- intraday has it. */
static void test_generation_evening(void)
{
    reset(false);
    s_gen_on = true;
    int64_t now = lt(2026, 10, 24, 19, 40), t0 = lt(2026, 10, 24, 0, 0);
    s_mixj.day = t0; s_mixj.last_ok = now; s_mixj.tomorrow = true;
    s_ec.day = t0; s_ec.last_ok = now; s_ec.last_try = now;
    static char bd[6][16384];
    for (int k = 0; k < 6; k++) {
        f_push(200, k % 2 ? gen_body2(bd[k], sizeof(bd[k]), t0, 192, 1000, 96, 192)    /* intraday: today */
                          : gen_body2(bd[k], sizeof(bd[k]), t0, 192, 3000, 0, 96));    /* day-ahead: tomorrow */
        price_feed_tick(now + k * PF_EC_SPACING);
        if (k == 0) {
            /* Nothing shown yet (a restart): the day-ahead part appears at
             * once, not only a minute later with intraday. */
            price_feed_get_mix(&gm);
            CHECK_F(gm.solar_gw.v[150], 3.150, 1e-4);
        }
    }
    price_feed_get_mix(&gm);
    int today = 0, tomorrow = 0;
    for (int i = 0; i < 96; i++) today += isfinite(gm.solar_gw.v[i]);
    for (int i = 96; i < 192; i++) tomorrow += isfinite(gm.solar_gw.v[i]);
    CHECK_I(today, 96);
    CHECK_I(tomorrow, 96);
    CHECK_F(gm.solar_gw.v[50], 1.050, 1e-4);
    CHECK_F(gm.solar_gw.v[150], 3.150, 1e-4);

    /* The next round's day-ahead part alone must not hide today again. */
    s_gen_stage = 0;
    f_push(200, gen_body2(bd[0], sizeof(bd[0]), t0, 192, 3000, 0, 96));
    s_buf = PF_BUF_ALLOC(PF_BUF_CAP);            /* as price_feed_tick does */
    gen_fetch(now + 3 * 3600);
    PF_BUF_FREE(s_buf);
    s_buf = NULL;
    price_feed_get_mix(&gm);
    CHECK(isfinite(gm.solar_gw.v[50]));
}

/* A failing stage backs off and the round continues where it stopped. */
static void test_generation_backoff(void)
{
    reset(false);
    s_gen_on = true;
    int64_t now = lt(2026, 10, 21, 10, 0), t0 = lt(2026, 10, 21, 0, 0);
    s_mixj.day = t0; s_mixj.last_ok = now;
    s_ec.day = t0; s_ec.last_ok = now; s_ec.last_try = now;
    f_push(200, gen_body(b1, sizeof(b1), t0, 96, 10000));
    f_push(429, "");
    price_feed_tick(now);
    int c = f_calls;
    price_feed_tick(now + PF_EC_SPACING);              /* solar intraday: 429 */
    CHECK_I(f_calls, c + 1);
    CHECK_I(s_gen_stage, 1);
    for (int64_t k = now + 2 * PF_EC_SPACING; k < now + 3000; k += 30) price_feed_tick(k);
    CHECK_I(f_calls, c + 1);                           /* held off */
    f_push(200, gen_body(b2, sizeof(b2), t0, 96, 2000));
    for (int64_t k = now + 3000; k < now + 3 * 3600; k += 30) { price_feed_tick(k); if (s_gen_stage == 2) break; }
    CHECK(url_has("production_type=solar&forecast_type=intraday"));   /* resumed at the failed stage */
}

static void test_dst(void)
{
    reset(true);
    int64_t t0 = lt(2026, 10, 25, 0, 0), t1 = lt(2026, 10, 26, 0, 0);
    CHECK_I(t1 - t0, 25 * 3600);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 100, t1, 96, 900, false));
    price_feed_tick(lt(2026, 10, 25, 15, 0));
    price_feed_get(&ps);
    CHECK(ps.err[0] == '\0');
    CHECK(ps.tomorrow);
    CHECK_I(ps.total.n, 192);
    CHECK_F(ps.total.v[99], 25.0 + 9.9, 1e-3);
    CHECK_F(ps.total.v[100], 30.0, 1e-3);
    CHECK_F(ps.total.v[191], 30.0 + 9.1, 1e-3);
    /* nothing ran past the end of total.v into its neighbour */
    CHECK_I(ps.energy.t0, t0);
    CHECK_I(ps.energy.n, 192);
    CHECK_F(ps.energy.v[191], 15.0 + 9.1, 1e-3);

    /* today only: 100 slots */
    reset(true);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 100, 0, 0, 900, false));
    price_feed_tick(lt(2026, 10, 25, 10, 0));
    price_feed_get(&ps);
    CHECK_I(ps.total.n, 100);
    CHECK_F(ps.total.v[99], 34.9, 1e-3);

    /* hourly on the long day: 25 hours, 100 slots, none missing */
    reset(true);
    f_push(200, tibber_body(b1, sizeof(b1), t0, 25, 0, 0, 3600, false));
    price_feed_tick(lt(2026, 10, 25, 10, 0));
    price_feed_get(&ps);
    CHECK_I(ps.total.n, 100);
    int nan = 0;
    for (int i = 0; i < 100; i++) nan += isnan(ps.total.v[i]);
    CHECK_I(nan, 0);
    CHECK_F(ps.total.v[99], 25.0 + 2.4, 1e-3);

    /* spring forward: 92 quarter hours */
    reset(true);
    int64_t m0 = lt(2026, 3, 29, 0, 0);
    f_push(200, tibber_body(b1, sizeof(b1), m0, 92, 0, 0, 900, false));
    price_feed_tick(lt(2026, 3, 29, 10, 0));
    price_feed_get(&ps);
    CHECK_I(ps.total.n, 92);
    CHECK_F(ps.total.v[91], 25.0 + 9.1, 1e-3);
}

static void test_no_clock(void)
{
    reset(true);
    f_push(200, "{}");
    price_feed_request();
    price_feed_tick(0);
    price_feed_tick(1600000000);
    price_feed_tick(PF_CLOCK_OK - 1);
    CHECK_I(f_calls, 0);
    pf_job_t j = { 0 };
    CHECK(price_due(&j, 1600000000) == INT64_MAX);
    CHECK(mix_due(&j, 1600000000) == INT64_MAX);
    price_feed_get(&ps);
    CHECK_I(ps.total.n, 0);
}

/* A Tibber answer bigger than the buffer: an error, not half a day. */
static void test_too_big(void)
{
    reset(true);
    static char huge[PF_BUF_CAP + 100];
    memset(huge, ' ', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    f_push(200, huge);
    price_feed_tick(lt(2026, 10, 21, 10, 0));
    price_feed_get(&ps);
    CHECK(strcmp(ps.err, "Tibber: Antwort zu groß") == 0);
}

int main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    RUN(test_before_first_tick);
    RUN(test_iso);
    RUN(test_tibber_today);
    RUN(test_tibber_tomorrow);
    RUN(test_tibber_before_t0);
    RUN(test_tibber_hourly);
    RUN(test_tibber_errors);
    RUN(test_energy_charts);
    RUN(test_fallback);
    RUN(test_no_fallback_over_tibber);
    RUN(test_mix);
    RUN(test_schedule_price);
    RUN(test_schedule_mix);
    RUN(test_schedule_tick);
    RUN(test_generation);
    RUN(test_generation_evening);
    RUN(test_generation_backoff);
    RUN(test_dst);
    RUN(test_no_clock);
    RUN(test_too_big);
    return t_report("price_feed");
}

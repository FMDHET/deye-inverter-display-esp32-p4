#include "price_feed.h"
#include "em_cfg.h"
#include "https_get.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* The response buffer lives in PSRAM and only during a fetch; the state
 * block (pf_big_t) lives there for good. The host test has no heap_caps; it
 * defines these three before including this file. */
#ifndef PF_BUF_ALLOC
#include "esp_heap_caps.h"
#define PF_BUF_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define PF_BUF_FREE(p)  heap_caps_free(p)
#define PF_BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#endif

static const char *TAG = "price_feed";

#define PF_CLOCK_OK       1700000000LL     /* before this the clock is not set  */
#define PF_BUF_CAP        (48 * 1024)      /* Tibber answers ~20 kB for 2 days   */
#define PF_MAX_ENT        320              /* entries of one answer we keep      */
#define PF_VAT            1.19f
#define PF_OFFSET_DEF     20.0f            /* ct, fees + taxes before Tibber told us */
#define PF_ROLL_PX        (5 * 60)         /* new price day starts at 00:05      */
#define PF_ROLL_MIX       (10 * 60)        /* new mix day at 00:10               */
#define PF_REFRESH_PX     (6 * 3600)
#define PF_REFRESH_MIX    (3 * 3600)
#define PF_EC_GAP         (2 * 3600)       /* Energy-Charts prices at most this often */
#define PF_TMRW_FROM_H    13               /* day-ahead auction results ~12:45   */
#define PF_TMRW_UNTIL_H   21
#define PF_TMRW_EVERY     (30 * 60)
#define PF_429_HOLD       3600
/* Energy-Charts publishes tomorrow's grid mix and generation forecast some
 * time in the afternoon or evening (not yet at 15:24 on 2026-10-25), while
 * the 3-h refresh could leave tomorrow empty for hours after that. Until
 * tomorrow is in, ask again this often between these hours. */
#define PF_MIX_TMRW_FROM_H  12
#define PF_MIX_TMRW_UNTIL_H 23
#define PF_MIX_TMRW_EVERY   (45 * 60)

#define TIBBER_URL   "https://api.tibber.com/v1-beta/gql"
#define TIBBER_QUERY "{\"query\":\"{ viewer { homes { id currentSubscription { "  \
                     "priceInfo(resolution: QUARTER_HOURLY) { "                    \
                     "today { total energy startsAt } "                           \
                     "tomorrow { total energy startsAt } } } } } }\"}"
#define EC_PRICE_URL "https://api.energy-charts.info/price?bzn=DE-LU&start=%s&end=%s"
#define EC_MIX_URL   "https://api.energy-charts.info/ren_share_forecast?country=de"
#define EC_GEN_URL   "https://api.energy-charts.info/public_power_forecast?country=de&production_type=%s&forecast_type=%s"
/* Generation forecast: per production type first "day-ahead", then
 * "intraday". Day-ahead covers tomorrow -- and today only until tomorrow's
 * is published in the afternoon: from then on its today-slots come back
 * null (seen 2026-10-24, 19:41: today's wind and sun gone after a restart).
 * Intraday covers today. So day-ahead fills, intraday overwrites today with
 * the newer numbers; a null never overwrites a value. */
#define GEN_STAGES   6
/* Energy-Charts answers 429 after a handful of calls in a short time. With
 * prices (fallback), the mix and three generation forecasts all going there,
 * any two of its requests are kept at least this far apart. */
#define PF_EC_SPACING     60

/* One source's fetch state. Only the energy manager task touches it. */
typedef struct {
    int64_t day;        /* local midnight of the day the last good data starts on, 0 = none */
    bool    tomorrow;   /* ... and it covers the next day as well                          */
    int64_t last_try;   /* last attempt, good or not, 0 = never                            */
    int64_t last_ok;
    uint8_t fails;      /* consecutive failures, drives the backoff                       */
    bool    r429;       /* the last failure was HTTP 429                                   */
    char    err[80];
} pf_job_t;

_Static_assert(sizeof(((pf_job_t *)0)->err) == sizeof(((grid_mix_t *)0)->err), "err sizes");

/* Everything big lives in one PSRAM block, allocated on the first tick:
 * ~19 kB that would otherwise sit in internal .bss (external .bss is not
 * enabled in this build), the RAM esp_hosted competes for. Before the block
 * exists the getters hand out an empty set. */
typedef struct {
    price_set_t px;                 /* published, guarded by s_mux          */
    grid_mix_t  mix;
    price_set_t wpx;                /* built by the tick, then copied out   */
    grid_mix_t  wmix;
    int64_t     ts[PF_MAX_ENT];     /* the entries of one answer, before    */
    int32_t     span[PF_MAX_ENT];   /* they go onto the slot grid           */
    float       va[PF_MAX_ENT], vb[PF_MAX_ENT];
    double      num[PF_MAX_ENT];
} pf_big_t;

/* The getters run in the HTTP server task: copy under a spinlock. */
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static pf_big_t     *s_big;

/* Task-only state. Static, not on the stack: the caller has ~6 kB. */
static pf_job_t      s_tib, s_ec, s_mixj, s_genj;
static int           s_gen_stage;       /* next fetch: type = stage / 2, day-ahead (even) or intraday (odd) */
static bool          s_gen_on = true;   /* host tests of the price/mix sequence turn it off */
static int64_t       s_ec_last;         /* last request to Energy-Charts       */
static em_cfg_t      s_cfg;
static char         *s_buf;
static int           s_n;               /* entries in s_big->ts/va/vb */
static float         s_offset = PF_OFFSET_DEF;
static bool          s_offset_learned;
static volatile bool s_force_px, s_force_mix;

/* ------------------------------------------------------------ local time
 * TZ is set by the NTP module. Midnights and 13:00 go through mktime so a
 * DST day comes out with its 92 or 100 quarter hours. */
static int64_t local_at(int64_t t, int days, int hour)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_mday += days;
    tm.tm_hour  = hour;
    tm.tm_min   = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}
static int64_t local_midnight(int64_t t) { return local_at(t, 0, 0); }

static void local_date(int64_t t, char *out, size_t len)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, len, "%Y-%m-%d", &tm);
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

/* "2026-10-21T00:00:00.000+02:00" -> unix seconds. The offset comes from the
 * string, never from TZ: Tibber answers in the home's zone, and the firmware's
 * TZ could be anything the user picked. An offset is required. */
static bool iso_to_unix(const char *s, int64_t *out)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!s || strlen(s) < 19) return false;
    if (!digits(s, 4, &y) || s[4] != '-' || !digits(s + 5, 2, &mo) || s[7] != '-' ||
        !digits(s + 8, 2, &d) || (s[10] != 'T' && s[10] != ' ') || !digits(s + 11, 2, &h) ||
        s[13] != ':' || !digits(s + 14, 2, &mi) || s[16] != ':' || !digits(s + 17, 2, &se))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
    const char *p = s + 19;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    int off = 0;
    if (*p == 'Z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1, oh = 0, om = 0;
        p++;
        if (!digits(p, 2, &oh)) return false;
        p += 2;
        if (*p == ':') p++;
        if (*p >= '0' && *p <= '9') {
            if (!digits(p, 2, &om)) return false;
            p += 2;
        }
        if (oh > 14 || om > 59) return false;
        off = sign * (oh * 3600 + om * 60);
    } else {
        return false;
    }
    if (*p != '\0') return false;
    *out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off;
    return true;
}

/* ------------------------------------------------------------ JSON scanning
 * cJSON builds one heap node per value, and with SPIRAM_MALLOC_ALWAYSINTERNAL
 * at 16 kB every node lands in internal RAM: a 2-day Tibber answer is ~800
 * nodes, ~70 kB -- the RAM esp_hosted's SDIO buffers once starved for. So the
 * answer is walked as text, and cJSON only ever sees one small entry at a
 * time (or the short "errors" array). */
static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *skip_string(const char *p, const char *e)
{
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

/* Just past the JSON value at p, NULL when it is cut off. */
static const char *skip_value(const char *p, const char *e)
{
    p = skip_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') return skip_string(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') {
                p = skip_string(p, e);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n')
        p++;
    return p > s ? p : NULL;
}

/* The value of the first "key": in [p, e), or NULL. */
static const char *find_key(const char *p, const char *e, const char *key)
{
    size_t kl = strlen(key);
    for (; p && p + kl + 2 < e; p++) {
        if (*p != '"' || memcmp(p + 1, key, kl) != 0 || p[kl + 1] != '"') continue;
        const char *q = skip_ws(p + kl + 2, e);
        if (q < e && *q == ':') {
            q = skip_ws(q + 1, e);
            return q < e ? q : NULL;
        }
    }
    return NULL;
}

/* Walk an array: *pp starts at its '['. Returns the next element (length in
 * *len), NULL at the end; *ok turns false on malformed input. */
static const char *arr_next(const char **pp, const char *e, size_t *len, bool *ok)
{
    const char *p = skip_ws(*pp, e);
    if (p < e && *p == '[') {
        p = skip_ws(p + 1, e);
    } else if (p < e && *p == ',') {
        p = skip_ws(p + 1, e);
    }
    if (p >= e) { *ok = false; return NULL; }
    if (*p == ']') { *pp = p; return NULL; }
    const char *v  = p;
    const char *ve = skip_value(p, e);
    if (!ve) { *ok = false; return NULL; }
    p = skip_ws(ve, e);
    if (p >= e || (*p != ',' && *p != ']')) { *ok = false; return NULL; }
    *pp  = p;
    *len = (size_t)(ve - v);
    return v;
}

/* The numbers of the array under `key` (null -> NAN) into s_big->num. Returns the
 * count, -1 when the key is missing or the array malformed. */
static int num_array(const char *b, const char *e, const char *key)
{
    const char *p = find_key(b, e, key);
    if (!p || *p != '[') return -1;
    int  n  = 0;
    bool ok = true;
    size_t len;
    const char *v;
    while ((v = arr_next(&p, e, &len, &ok)) != NULL) {
        double d;
        if (len == 4 && memcmp(v, "null", 4) == 0) {
            d = NAN;
        } else {
            char *end;
            d = strtod(v, &end);
            if (end != v + len) return -1;
        }
        if (n < PF_MAX_ENT) s_big->num[n++] = d;
    }
    return ok ? n : -1;
}

/* ------------------------------------------------------------ slot grid */
static void series_clear(em_series_t *s, int64_t t0)
{
    s->t0 = t0;
    s->n  = 0;
    for (int i = 0; i < EM_MAX_SLOTS; i++) s->v[i] = NAN;
}

/* Slots from t0 to t1, clamped to the grid: a 2-day window from the day
 * clocks go back has 196 quarter hours, the grid holds 192. */
static uint16_t slots(int64_t t0, int64_t t1)
{
    int64_t n = (t1 - t0) / EM_SLOT_S;
    return (uint16_t)(n < 0 ? 0 : (n > EM_MAX_SLOTS ? EM_MAX_SLOTS : n));
}

static void series_put(em_series_t *s, int64_t t, int32_t span, float v)
{
    if (!isfinite(v)) return;
    t = em_slot_floor(t);
    for (int32_t k = 0; k < span / EM_SLOT_S; k++) {
        int64_t tk = t + (int64_t)k * EM_SLOT_S;
        if (tk < s->t0) continue;
        int64_t i = (tk - s->t0) / EM_SLOT_S;
        if (i >= EM_MAX_SLOTS) return;
        s->v[i] = v;
    }
}

static bool any_finite(const em_series_t *s, int from, int to)
{
    for (int i = from; i < to && i < EM_MAX_SLOTS; i++)
        if (isfinite(s->v[i])) return true;
    return false;
}

/* Set n and blank what lies beyond it. */
static void series_cut(em_series_t *s, uint16_t n)
{
    s->n = n;
    for (int i = n; i < EM_MAX_SLOTS; i++) s->v[i] = NAN;
}

/* How long each entry lasts: the distance to the next one when that is a
 * plausible step (a quarter hour up to an hour), else the step before.
 * Tibber has been seen answering hourly despite QUARTER_HOURLY, and older
 * Energy-Charts data is hourly: an hourly value then covers its four
 * quarter hours instead of one quarter hour followed by three gaps. */
static void compute_spans(void)
{
    int32_t last = EM_SLOT_S;
    for (int i = 0; i < s_n; i++) {
        int32_t sp = last;
        if (i + 1 < s_n) {
            int64_t d = s_big->ts[i + 1] - s_big->ts[i];
            if (d >= EM_SLOT_S && d <= 3600 && d % EM_SLOT_S == 0) sp = (int32_t)d;
        }
        s_big->span[i] = sp;
        last      = sp;
    }
}

/* Put s_big->ts/s_big->va(/s_big->vb) onto total(/energy) for the day starting at t0 and
 * check the result. Returns false when today has no value at all. */
static bool fill_prices(int64_t t0, em_series_t *total, em_series_t *energy, bool *tomorrow)
{
    compute_spans();
    series_clear(total, t0);
    series_clear(energy, t0);
    for (int i = 0; i < s_n; i++) {
        series_put(total,  s_big->ts[i], s_big->span[i], s_big->va[i]);
        series_put(energy, s_big->ts[i], s_big->span[i], s_big->vb[i]);
    }
    int i1 = slots(t0, local_at(t0, 1, 0));
    if (!any_finite(total, 0, i1)) return false;
    *tomorrow = any_finite(total, i1, EM_MAX_SLOTS);
    uint16_t n = *tomorrow ? slots(t0, local_at(t0, 2, 0)) : (uint16_t)i1;
    series_cut(total, n);
    series_cut(energy, n);
    return true;
}

/* ------------------------------------------------------------ schedule
 * Pure functions of the job state and the clock (and TZ), so the test can
 * walk them through a day without any HTTP. */
static int64_t backoff_s(const pf_job_t *j)
{
    if (!j->fails) return 0;
    int64_t b = j->fails == 1 ? 300 : j->fails == 2 ? 600 : j->fails == 3 ? 1200 : 3600;
    if (j->r429 && b < PF_429_HOLD) b = PF_429_HOLD;
    return b;
}

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

/* When the next price fetch is due (<= now: fetch now). */
static int64_t price_due(const pf_job_t *j, int64_t now)
{
    if (now < PF_CLOCK_OK) return INT64_MAX;
    /* Until 00:05 it is still yesterday for the schedule: Tibber flips its
     * "today" a moment after midnight, and yesterday's tomorrow covers the
     * gap. */
    int64_t day = local_midnight(now - PF_ROLL_PX);
    int64_t due;
    if (j->day < day) {
        due = now;                                     /* nothing for today */
    } else {
        due = j->last_ok + PF_REFRESH_PX;
        if (!j->tomorrow) {
            int64_t d    = max64(j->day, day);
            int64_t from = local_at(d, 0, PF_TMRW_FROM_H);
            int64_t till = local_at(d, 0, PF_TMRW_UNTIL_H);
            int64_t next = max64(from, j->last_try + PF_TMRW_EVERY);
            if (next < till) due = min64(due, next);
        }
    }
    if (j->fails) due = max64(due, j->last_try + backoff_s(j));
    return due;
}

/* The Energy-Charts price fallback: the same rules, but never more often
 * than every two hours -- it answers 429 after a handful of calls. */
static int64_t ec_due(const pf_job_t *j, int64_t now)
{
    int64_t due = price_due(j, now);
    if (j->last_try) due = max64(due, j->last_try + PF_EC_GAP);
    return due;
}

/* Grid mix: every 3 h, new day at 00:10; from noon on every 45 min until
 * tomorrow is in (see PF_MIX_TMRW_*). */
static int64_t mix_due(const pf_job_t *j, int64_t now)
{
    if (now < PF_CLOCK_OK) return INT64_MAX;
    int64_t day = local_midnight(now - PF_ROLL_MIX);
    int64_t due = j->day < day ? now : j->last_ok + PF_REFRESH_MIX;
    if (j->day >= day && !j->tomorrow) {
        int64_t from = local_at(day, 0, PF_MIX_TMRW_FROM_H);
        int64_t till = local_at(day, 0, PF_MIX_TMRW_UNTIL_H);
        int64_t next = max64(from, j->last_try + PF_MIX_TMRW_EVERY);
        if (now < till && next < till) due = min64(due, next);   /* not after the window */
    }
    if (j->fails) due = max64(due, j->last_try + backoff_s(j));
    return due;
}

/* Does the series reach into tomorrow (relative to its own day t0)? */
static bool has_tomorrow(const em_series_t *s, int64_t t0)
{
    int64_t t1 = local_at(t0, 1, 0), t2 = local_at(t0, 2, 0);
    return any_finite(s, slots(t0, t1), slots(t0, t2));
}

/* Generation forecast: same cadence as the mix, GEN_STAGES requests in a
 * row (one per tick, PF_EC_SPACING apart); the job counts as done after the
 * last. */
static int64_t gen_due(const pf_job_t *j, int64_t now)
{
    return mix_due(j, now);
}

/* A forced refresh skips the schedule, not the backoff (nor a 429 hold). */
static int64_t forced_due(const pf_job_t *j, int64_t now)
{
    return j->fails ? j->last_try + backoff_s(j) : now;
}

/* Energy-Charts stands in for Tibber when there is no token, or when Tibber
 * fails and has nothing for today. With Tibber's today at hand a missing
 * tomorrow is left to Tibber's retries: an estimated end price must not
 * overwrite a real one. */
static bool ec_needed(bool tok, int64_t now)
{
    return !tok || (s_tib.fails && s_tib.day < local_midnight(now - PF_ROLL_PX));
}

/* ------------------------------------------------------------ results */
/* Job state is written here only, under s_mux, so price_feed_sources()
 * (HTTP task) never reads a half-written error text. */
static void job_fail(pf_job_t *j, int status, const char *why)
{
    portENTER_CRITICAL(&s_mux);
    if (j->fails < 255) j->fails++;
    j->r429 = status == 429;
    snprintf(j->err, sizeof(j->err), "%s", why);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "%s (attempt %u, next in %lld s)", why, j->fails, (long long)backoff_s(j));
}

static void job_ok(pf_job_t *j, int64_t now, int64_t day, bool tomorrow)
{
    portENTER_CRITICAL(&s_mux);
    j->fails    = 0;
    j->r429     = false;
    j->err[0]   = '\0';
    j->last_ok  = now;
    j->day      = day;
    j->tomorrow = tomorrow;
    portEXIT_CRITICAL(&s_mux);
}

static const char *http_err(const char *who, esp_err_t rc, int status, char *out, size_t len)
{
    if (status == 429)                  snprintf(out, len, "%s: zu viele Anfragen (HTTP 429)", who);
    else if (rc == ESP_ERR_INVALID_SIZE) snprintf(out, len, "%s: Antwort zu groß", who);
    else if (rc == ESP_ERR_NO_MEM)      snprintf(out, len, "%s: verschoben, Speicher gerade knapp", who);
    else if (status == 0)               snprintf(out, len, "%s: keine Verbindung", who);
    else                                snprintf(out, len, "%s: HTTP %d", who, status);
    return out;
}

/* The error text shown for the prices: Tibber's when it is configured and
 * failing (that is the one the user can fix), else the fallback's. */
static const char *px_err(bool tok)
{
    if (tok && s_tib.fails) return s_tib.err;
    if (s_ec.fails && (!tok || s_tib.fails)) return s_ec.err;
    return "";
}

static void publish_err(bool tok)
{
    const char *e = px_err(tok);
    portENTER_CRITICAL(&s_mux);
    snprintf(s_big->px.err, sizeof(s_big->px.err), "%s", e);
    portEXIT_CRITICAL(&s_mux);
}

static void publish_px(int64_t now, const char *src, bool tomorrow, bool tok)
{
    snprintf(s_big->wpx.src, sizeof(s_big->wpx.src), "%s", src);
    snprintf(s_big->wpx.err, sizeof(s_big->wpx.err), "%s", px_err(tok));
    s_big->wpx.fetched  = now;
    s_big->wpx.tomorrow = tomorrow;
    portENTER_CRITICAL(&s_mux);
    s_big->px = s_big->wpx;
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------ Tibber */

/* Text from the outside world goes into a page: no control characters,
 * quotes or backslashes. */
static void tidy(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == '"' || *s == '\\') *s = ' ';
}

static bool has_ci(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; s && *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && (s[i] | 0x20) == (needle[i] | 0x20)) i++;
        if (i == n) return true;
    }
    return false;
}

/* A GraphQL "errors" array: an invalid token comes back this way too. */
static void tibber_gql_err(const char *arr, const char *e, char *out, size_t len)
{
    const char *ae  = skip_value(arr, e);
    cJSON      *a   = ae ? cJSON_ParseWithLength(arr, (size_t)(ae - arr)) : NULL;
    cJSON      *e0  = cJSON_GetArrayItem(a, 0);
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e0, "message"));
    const char *code = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(e0, "extensions"), "code"));
    if ((code && has_ci(code, "UNAUTHENTICATED")) || (msg && has_ci(msg, "token")))
        snprintf(out, len, "Tibber: Token ungültig");
    else if (msg)
        snprintf(out, len, "Tibber: %.60s", msg);
    else
        snprintf(out, len, "Tibber: Fehler in der Antwort");
    cJSON_Delete(a);
    tidy(out);
}

/* One array of {total, energy, startsAt} into s_big->ts/s_big->va/s_big->vb (ct/kWh). */
static bool tibber_entries(const char *arr, const char *e)
{
    bool ok = true;
    size_t len;
    const char *v, *p = arr;
    while ((v = arr_next(&p, e, &len, &ok)) != NULL) {
        cJSON *o = cJSON_ParseWithLength(v, len);
        if (!cJSON_IsObject(o)) { cJSON_Delete(o); return false; }
        cJSON  *tt = cJSON_GetObjectItem(o, "total");
        cJSON  *en = cJSON_GetObjectItem(o, "energy");
        int64_t t;
        if (iso_to_unix(cJSON_GetStringValue(cJSON_GetObjectItem(o, "startsAt")), &t) &&
            s_n < PF_MAX_ENT) {
            s_big->ts[s_n] = t;
            s_big->va[s_n] = cJSON_IsNumber(tt) ? (float)(tt->valuedouble * 100.0) : NAN;
            s_big->vb[s_n] = cJSON_IsNumber(en) ? (float)(en->valuedouble * 100.0) : NAN;
            s_n++;
        }
        cJSON_Delete(o);
    }
    return ok;
}

/* Parse a 200 answer into s_big->wpx. NULL on success, else the error text. */
static const char *tibber_parse(size_t blen, int64_t now, bool *tomorrow)
{
    const char *b = s_buf, *e = s_buf + blen;
    const char *homes = find_key(b, e, "homes");
    const char *he    = homes && *homes == '[' ? skip_value(homes, e) : NULL;
    if (!he) return "Tibber: unerwartete Antwort";

    /* The first home with a contract; a home without one has null here. */
    const char *sub = NULL, *se = NULL, *p = homes;
    while ((p = find_key(p, he, "currentSubscription")) != NULL) {
        if (*p == '{') { sub = p; se = skip_value(p, he); break; }
        p++;
    }
    if (!sub || !se) return "Tibber: kein Vertrag mit Preisen gefunden";

    const char *today = find_key(sub, se, "today");
    const char *tmrw  = find_key(sub, se, "tomorrow");
    if (!today || *today != '[') return "Tibber: keine Preise geliefert";
    s_n = 0;
    if (!tibber_entries(today, se) || (tmrw && *tmrw == '[' && !tibber_entries(tmrw, se)))
        return "Tibber: unerwartete Antwort";

    if (!fill_prices(local_midnight(now), &s_big->wpx.total, &s_big->wpx.energy, tomorrow))
        return "Tibber: keine Preise für heute";
    return NULL;
}

/* Fees and taxes on top of the exchange price, as Tibber bills them right
 * now: what the Energy-Charts fallback adds to its exchange price. */
static void learn_offset(void)
{
    double sum = 0.0;
    int    n   = 0;
    for (int i = 0; i < s_n; i++)
        if (isfinite(s_big->va[i]) && isfinite(s_big->vb[i])) {
            sum += s_big->va[i] - s_big->vb[i] * PF_VAT;
            n++;
        }
    if (n) {
        s_offset         = (float)(sum / n);
        s_offset_learned = true;
    }
}

static void tibber_fetch(int64_t now, bool tok)
{
    pf_job_t *j = &s_tib;
    size_t    len = 0;
    int       status = 0;
    char      why[80];
    esp_err_t rc = https_get(&(https_req_t){ .url         = TIBBER_URL,
                                             .auth_bearer = s_cfg.tibber_token,
                                             .post_json   = TIBBER_QUERY,
                                             .timeout_ms  = 20000 },
                             s_buf, PF_BUF_CAP, &len, &status);
    j->last_try = now;

    const char *errs = len ? find_key(s_buf, s_buf + len, "errors") : NULL;
    bool tomorrow = false;
    const char *bad = NULL;
    if (status == 401 || status == 403) {
        bad = "Tibber: Token ungültig";
    } else if (errs && *errs == '[') {
        tibber_gql_err(errs, s_buf + len, why, sizeof(why));
        bad = why;
    } else if (rc != ESP_OK) {
        bad = http_err("Tibber", rc, status, why, sizeof(why));
    } else {
        bad = tibber_parse(len, now, &tomorrow);
    }
    if (bad) {
        job_fail(j, status, bad);
        publish_err(tok);
        return;
    }
    job_ok(j, now, s_big->wpx.total.t0, tomorrow);
    learn_offset();
    publish_px(now, "tibber", tomorrow, tok);
    ESP_LOGI(TAG, "Tibber: %d prices, %u slots, tomorrow %s, fees %.2f ct",
             s_n, s_big->wpx.total.n, tomorrow ? "yes" : "no", (double)s_offset);
}

/* ------------------------------------------------------------ Energy-Charts */
static void ec_price_fetch(int64_t now, bool tok)
{
    pf_job_t *j  = &s_ec;
    int64_t   t0 = local_midnight(now);
    char      d0[12], d1[12], url[128], why[80];
    local_date(t0, d0, sizeof(d0));
    local_date(local_at(t0, 1, 0), d1, sizeof(d1));
    snprintf(url, sizeof(url), EC_PRICE_URL, d0, d1);

    size_t len = 0;
    int    status = 0;
    esp_err_t rc = https_get(&(https_req_t){ .url = url, .timeout_ms = 20000 },
                             s_buf, PF_BUF_CAP, &len, &status);
    j->last_try = now;

    bool tomorrow = false;
    const char *bad = NULL;
    if (rc != ESP_OK) {
        bad = http_err("Energy-Charts", rc, status, why, sizeof(why));
    } else {
        const char *e = s_buf + len;
        int nt = num_array(s_buf, e, "unix_seconds");
        for (int i = 0; i < nt; i++) s_big->ts[i] = isfinite(s_big->num[i]) ? (int64_t)s_big->num[i] : 0;
        int np = num_array(s_buf, e, "price");
        if (nt < 0 || np < 0) {
            bad = "Energy-Charts: unerwartete Antwort";
        } else {
            /* EUR/MWh -> ct/kWh is /10. No end price here: VAT on the
             * exchange part plus the fees Tibber last showed us. */
            s_n = nt < np ? nt : np;
            for (int i = 0; i < s_n; i++) {
                s_big->vb[i] = isfinite(s_big->num[i]) && s_big->ts[i] ? (float)(s_big->num[i] / 10.0) : NAN;
                s_big->va[i] = s_big->vb[i] * PF_VAT + s_offset;
            }
            if (!fill_prices(t0, &s_big->wpx.total, &s_big->wpx.energy, &tomorrow))
                bad = "Energy-Charts: keine Preise für heute";
        }
    }
    if (bad) {
        job_fail(j, status, bad);
        publish_err(tok);
        return;
    }
    job_ok(j, now, t0, tomorrow);
    publish_px(now, "energy-charts", tomorrow, tok);
    ESP_LOGI(TAG, "Energy-Charts: %d prices, %u slots, tomorrow %s, +%.2f ct %s",
             s_n, s_big->wpx.total.n, tomorrow ? "yes" : "no", (double)s_offset,
             s_offset_learned ? "(from Tibber)" : "(default)");
}

static void mix_fetch(int64_t now)
{
    pf_job_t *j  = &s_mixj;
    int64_t   t0 = local_midnight(now);
    char      why[80];
    size_t    len = 0;
    int       status = 0;
    esp_err_t rc = https_get(&(https_req_t){ .url = EC_MIX_URL, .timeout_ms = 20000 },
                             s_buf, PF_BUF_CAP, &len, &status);
    j->last_try = now;

    const char *bad = NULL;
    if (rc != ESP_OK) {
        bad = http_err("Energy-Charts", rc, status, why, sizeof(why));
    } else {
        static const char *const keys[4] = { "ren_share", "solar_share",
                                             "wind_onshore_share", "wind_offshore_share" };
        em_series_t *ser[4] = { &s_big->wmix.ren, &s_big->wmix.solar, &s_big->wmix.wind_on, &s_big->wmix.wind_off };
        const char  *e  = s_buf + len;
        int nt = num_array(s_buf, e, "unix_seconds");
        s_n = nt < 0 ? 0 : nt;
        for (int i = 0; i < s_n; i++) s_big->ts[i] = isfinite(s_big->num[i]) ? (int64_t)s_big->num[i] : 0;
        compute_spans();
        uint16_t n = slots(t0, local_at(t0, 1, 0));
        for (int k = 0; k < 4; k++) {
            series_clear(ser[k], t0);
            int nv = num_array(s_buf, e, keys[k]);
            for (int i = 0; i < nv && i < s_n; i++)
                if (s_big->ts[i]) series_put(ser[k], s_big->ts[i], s_big->span[i], (float)s_big->num[i]);
            /* Today, or further when the forecast reaches further. */
            for (int i = EM_MAX_SLOTS - 1; i >= n; i--)
                if (isfinite(ser[k]->v[i])) { n = (uint16_t)(i + 1); break; }
        }
        for (int k = 0; k < 4; k++) series_cut(ser[k], n);
        if (nt < 0) bad = "Energy-Charts: unerwartete Antwort";
        else if (!any_finite(&s_big->wmix.ren, 0, slots(t0, local_at(t0, 1, 0))))
            bad = "Energy-Charts: keine Netzmix-Daten für heute";
    }
    if (bad) {
        job_fail(j, status, bad);
        portENTER_CRITICAL(&s_mux);
        memcpy(s_big->mix.err, j->err, sizeof(s_big->mix.err));   /* same size, terminated */
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    job_ok(j, now, t0, has_tomorrow(&s_big->wmix.ren, t0));
    s_big->wmix.fetched = now;
    s_big->wmix.err[0]  = '\0';
    portENTER_CRITICAL(&s_mux);
    s_big->mix = s_big->wmix;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "grid mix: %u slots", s_big->wmix.ren.n);
}

static void gen_fetch(int64_t now)
{
    /* Sun first: after a restart it shows a minute after the round starts,
     * not five. */
    static const char *const types[3] = { "solar", "wind_onshore", "wind_offshore" };
    em_series_t *ser[3] = { &s_big->wmix.solar_gw, &s_big->wmix.wind_on_gw, &s_big->wmix.wind_off_gw };
    pf_job_t *j  = &s_genj;
    int       k  = s_gen_stage / 2;
    bool      intraday = s_gen_stage % 2;
    int64_t   t0 = local_midnight(now);
    char      url[200], why[80];
    snprintf(url, sizeof(url), EC_GEN_URL, types[k], intraday ? "intraday" : "day-ahead");
    size_t len = 0;
    int    status = 0;
    esp_err_t rc = https_get(&(https_req_t){ .url = url, .timeout_ms = 20000 },
                             s_buf, PF_BUF_CAP, &len, &status);
    j->last_try = now;

    const char *bad = NULL;
    if (rc != ESP_OK) {
        bad = http_err("Energy-Charts", rc, status, why, sizeof(why));
    } else {
        const char *e = s_buf + len;
        int nt = num_array(s_buf, e, "unix_seconds");
        s_n = nt < 0 ? 0 : nt;
        for (int i = 0; i < s_n; i++) s_big->ts[i] = isfinite(s_big->num[i]) ? (int64_t)s_big->num[i] : 0;
        compute_spans();
        if (!intraday) series_clear(ser[k], t0);    /* intraday adds to what day-ahead left */
        int nv = num_array(s_buf, e, "forecast_values");
        uint16_t n = slots(t0, local_at(t0, 1, 0));
        for (int i = 0; i < nv && i < s_n; i++)
            if (s_big->ts[i] && isfinite(s_big->num[i]))
                series_put(ser[k], s_big->ts[i], s_big->span[i], (float)(s_big->num[i] / 1000.0));  /* MW -> GW */
        for (int i = EM_MAX_SLOTS - 1; i >= n; i--)
            if (isfinite(ser[k]->v[i])) { n = (uint16_t)(i + 1); break; }
        series_cut(ser[k], n);
        if (nt < 0 || nv < 0) bad = "Energy-Charts: unerwartete Antwort (Erzeugungsprognose)";
    }
    if (bad) {
        job_fail(j, status, bad);
        ESP_LOGW(TAG, "generation %s %s: %s", types[k], intraday ? "intraday" : "day-ahead", bad);
        return;
    }
    /* Both parts in: publish this type. The day-ahead part alone only when
     * nothing of the type is shown yet (after a restart) -- otherwise it
     * would hide today's intraday values for a minute in the evening. */
    em_series_t *pub = k == 0 ? &s_big->mix.solar_gw : k == 1 ? &s_big->mix.wind_on_gw : &s_big->mix.wind_off_gw;
    portENTER_CRITICAL(&s_mux);
    if (intraday || pub->n == 0) {
        *pub = *ser[k];
        s_big->mix.gen_fetched = now;
    }
    portEXIT_CRITICAL(&s_mux);
    int known = 0;
    for (int i = 0; i < ser[k]->n; i++) known += isfinite(ser[k]->v[i]);
    ESP_LOGI(TAG, "generation %s %s: %d of %u slots known", types[k], intraday ? "intraday" : "day-ahead",
             known, ser[k]->n);
    if (++s_gen_stage >= GEN_STAGES) { /* all in: the job is done */
        s_gen_stage = 0;
        job_ok(j, now, t0, has_tomorrow(&s_big->wmix.solar_gw, t0) && has_tomorrow(&s_big->wmix.wind_on_gw, t0));
    } else {
        j->fails = 0;                  /* the next stage follows at once (spacing permitting) */
    }
}

/* ------------------------------------------------------------ day roll
 * At 00:05 yesterday's "tomorrow" becomes today: move the published prices
 * so slot 0 is today's midnight again. The schedule re-fetches right after;
 * should that fail, the shifted prices keep the planner going. */
static void day_roll(int64_t now)
{
    int64_t day = local_midnight(now - PF_ROLL_PX);
    portENTER_CRITICAL(&s_mux);
    if (s_big->px.total.n && s_big->px.total.t0 < day) {
        int64_t k = (day - s_big->px.total.t0) / EM_SLOT_S;
        uint16_t n = k >= s_big->px.total.n ? 0 : (uint16_t)(s_big->px.total.n - k);
        em_series_t *ss[2] = { &s_big->px.total, &s_big->px.energy };
        for (int s = 0; s < 2; s++) {
            if (n) memmove(ss[s]->v, ss[s]->v + k, n * sizeof(float));
            for (int i = n; i < EM_MAX_SLOTS; i++) ss[s]->v[i] = NAN;
            ss[s]->t0 = day;
            ss[s]->n  = n;
        }
        s_big->px.tomorrow = false;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------ public */
void price_feed_tick(int64_t now)
{
    if (now < PF_CLOCK_OK) return;          /* no clock, no day, no schedule */
    if (!s_big) {
        pf_big_t *b = PF_BIG_ALLOC(sizeof(pf_big_t));
        if (!b) {
            ESP_LOGE(TAG, "no %u bytes PSRAM for the state", (unsigned)sizeof(pf_big_t));
            return;
        }
        portENTER_CRITICAL(&s_mux);
        s_big = b;
        portEXIT_CRITICAL(&s_mux);
    }
    day_roll(now);
    em_cfg_get(&s_cfg);
    bool tok = s_cfg.tibber_token[0] != '\0';

    /* One request per tick: each can block up to its timeout, and the
     * energy manager task has other work in between. */
    enum { PF_NONE, PF_TIB, PF_EC, PF_MIX, PF_GEN } what = PF_NONE;
    bool fpx = s_force_px, fmix = s_force_mix;
    bool ec_ok = !s_ec_last || now - s_ec_last >= PF_EC_SPACING;
    /* a generation round in progress continues without waiting 3 h */
    bool gen_go = s_gen_stage > 0 ? now >= s_genj.last_try + (s_genj.fails ? backoff_s(&s_genj) : 0)
                                  : now >= (fmix ? forced_due(&s_genj, now) : gen_due(&s_genj, now));
    if (tok && now >= (fpx ? forced_due(&s_tib, now) : price_due(&s_tib, now)))
        what = PF_TIB;
    else if (ec_ok && ec_needed(tok, now) && now >= (fpx ? forced_due(&s_ec, now) : ec_due(&s_ec, now)))
        what = PF_EC;
    else if (ec_ok && now >= (fmix ? forced_due(&s_mixj, now) : mix_due(&s_mixj, now)))
        what = PF_MIX;
    else if (ec_ok && s_gen_on && gen_go)
        what = PF_GEN;
    if (what == PF_NONE) return;
    if (what != PF_TIB) s_ec_last = now;

    s_buf = PF_BUF_ALLOC(PF_BUF_CAP);
    if (!s_buf) {
        ESP_LOGE(TAG, "no %u bytes PSRAM for the answer", (unsigned)PF_BUF_CAP);
        return;
    }
    switch (what) {
    case PF_TIB: s_force_px = false;  tibber_fetch(now, tok);   break;
    case PF_EC:  s_force_px = false;  ec_price_fetch(now, tok); break;
    case PF_MIX: s_force_mix = false; mix_fetch(now);           break;
    case PF_GEN: gen_fetch(now);                                 break;
    default: break;
    }
    PF_BUF_FREE(s_buf);
    s_buf = NULL;
}

void price_feed_request(void)
{
    s_force_px  = true;
    s_force_mix = true;
}

void price_feed_get(price_set_t *out)
{
    portENTER_CRITICAL(&s_mux);
    if (s_big) *out = s_big->px;
    else       memset(out, 0, sizeof(*out));
    portEXIT_CRITICAL(&s_mux);
}

void price_feed_get_mix(grid_mix_t *out)
{
    portENTER_CRITICAL(&s_mux);
    if (s_big) *out = s_big->mix;
    else       memset(out, 0, sizeof(*out));
    portEXIT_CRITICAL(&s_mux);
}

void price_feed_sources(int64_t now, pf_src_t out[PF_SRC_N])
{
    pf_job_t j[PF_SRC_N];
    bool tok = s_cfg.tibber_token[0] != '\0';
    portENTER_CRITICAL(&s_mux);
    j[PF_SRC_TIBBER] = s_tib; j[PF_SRC_EC_PRICE] = s_ec; j[PF_SRC_MIX] = s_mixj; j[PF_SRC_GEN] = s_genj;
    int stage = s_gen_stage;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < PF_SRC_N; i++) {
        pf_src_t *o = &out[i];
        memset(o, 0, sizeof(*o));
        o->last_try = j[i].last_try; o->last_ok = j[i].last_ok;
        o->fails = j[i].fails; o->r429 = j[i].r429; o->tomorrow = j[i].tomorrow;
        snprintf(o->err, sizeof(o->err), "%s", j[i].err);
    }
    out[PF_SRC_TIBBER].used   = tok;
    out[PF_SRC_EC_PRICE].used = ec_needed(tok, now) || s_ec.last_try;
    out[PF_SRC_MIX].used      = true;
    out[PF_SRC_GEN].used      = s_gen_on;
    out[PF_SRC_GEN].stage     = (uint8_t)stage;
    if (now >= PF_CLOCK_OK) {
        if (tok) out[PF_SRC_TIBBER].next = price_due(&j[PF_SRC_TIBBER], now);
        if (ec_needed(tok, now)) out[PF_SRC_EC_PRICE].next = ec_due(&j[PF_SRC_EC_PRICE], now);
        out[PF_SRC_MIX].next = mix_due(&j[PF_SRC_MIX], now);
        out[PF_SRC_GEN].next = stage > 0 ? j[PF_SRC_GEN].last_try + PF_EC_SPACING
                                           + (j[PF_SRC_GEN].fails ? backoff_s(&j[PF_SRC_GEN]) : 0)
                                         : gen_due(&j[PF_SRC_GEN], now);
        for (int i = 0; i < PF_SRC_N; i++)
            if (out[i].next == INT64_MAX) out[i].next = 0;
            else if (out[i].next && out[i].next < now) out[i].next = now;   /* due: at the next turn */
    }
}

float price_feed_offset_ct(bool *learned)
{
    if (learned) *learned = s_offset_learned;
    return s_offset;
}

#include "load_profile.h"
#include "keep_ram.h"
#include "load_hist.h"       /* the long-term record and its average */
#include "modbus_tcp.h"      /* house_w: the live household balance */
#include "nvs_store.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "load_prof";

#define LP_ALPHA         0.15f     /* EMA weight of a new quarter hour            */
#define LP_COVER_MIN     0.70f     /* share of a slot that must carry samples     */
#define LP_DAY_MIN       77        /* 80 % of 96 slots -> the day counts          */
#define LP_MAX_W         30000.0f  /* plausibility clamp of one sample            */
#define LP_MAX_GAP_S     30        /* one sample stands for at most this long     */
#define LP_SAVE_EVERY_S  3600      /* flash wear: at most one write per hour      */
#define LP_RETRY_S       600       /* after a refused write                       */
#define LP_CLOCK_MIN     1700000000LL  /* before this the clock was never set     */
#define LP_NONE          0xFFFFu   /* stored: slot never learned                  */
#define LP_VER           1

/* The NVS record. Integer watts are plenty for a forecast and halve the
 * size against floats -- the record competes for the same partition as the
 * settings. `size` catches a record of another layout even when a future
 * version forgets to bump `ver`. The checksum sits LAST: the NVS getter
 * cannot tell us how long the stored blob was, and a shorter one leaves the
 * tail of our zeroed buffer as it was -- so a truncated record ends in a
 * zero checksum, which does not match. */
typedef struct {
    uint16_t ver;
    uint16_t size;
    uint16_t days_wd, days_we;
    uint16_t w[2][LP_SLOTS];       /* [0] weekday, [1] weekend; LP_NONE = unknown */
    uint32_t sum;                  /* FNV-1a over everything above               */
} lp_rec_t;

static uint32_t rec_sum(const lp_rec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(lp_rec_t, sum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ---- shared with the getters: under s_mux --------------------------------
 * Watts as float, NAN = never learned. Kept in float so the EMA does not
 * stall on rounding: 0.15 of a 3 W difference would vanish in integers. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float    s_prof[2][LP_SLOTS];
static uint16_t s_days[2];
static uint32_t s_gen;             /* bumped on every change of the above      */

/* ---- tick-side state: only the energy manager task touches it ----------- */
static int64_t  s_slot_t;          /* start of the slot being accumulated, 0 = none */
static uint8_t  s_slot_idx;        /* its local index 0..95                         */
static uint8_t  s_slot_we;         /* its day type, 1 = weekend                     */
static int32_t  s_slot_day;        /* its local day key                             */
static int32_t  s_slot_hday;       /* its date as a load_hist day key               */
static double   s_sum;             /* sum of W * seconds                            */
static int32_t  s_cov;             /* seconds carried by samples                    */
static int64_t  s_last_tick;       /* previous tick, 0 = none                       */

static int32_t  s_day_key;         /* local day whose slots are being counted       */
static uint8_t  s_day_we;
static uint8_t  s_day_seen[LP_SLOTS / 8];   /* distinct slots learned that day     */

/* The day being measured, slot by slot, for the long-term record. In memory
 * that a software restart does not clear (keep_ram.h, next to the PV
 * measurement): an OTA
 * in the afternoon must not cost the day its morning. Valid only with magic,
 * size, checksum -- and for the date it names. */
#define LP_KEEP_MAGIC 0x4C504B31u   /* "LPK1" */
typedef struct {
    uint32_t magic, size;
    int32_t  day;                   /* load_hist day key, 0 = none          */
    uint8_t  wday, _rsv[3];
    float    sum[LP_SLOTS];         /* sum of slot means (a 25-h day has two) */
    uint8_t  cnt[LP_SLOTS];
    uint32_t crc;
} lp_keep_t;
/* In the LP RAM object of keep_ram.h (a stable address from build to build). */
_Static_assert(sizeof(lp_keep_t) <= sizeof(g_keep_ram.lp), "lp_keep_t fits its keep area");
#define s_keep (*(lp_keep_t *)(void *)g_keep_ram.lp)

/* Long-term averages, under s_mux. */
static EXT_RAM_BSS_ATTR float s_avg[2][LP_SLOTS];
static uint16_t s_avg_days[2];
static int      s_hist_count;
static int32_t  s_hist_oldest;
static bool     s_hist_ok;

static int64_t  s_save_due;        /* earliest time of the next write               */
static uint32_t s_saved_gen;       /* s_gen of the last successful write            */

typedef struct { uint8_t idx, we, wday; int32_t day, hday; } lp_where_t;

/* Days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil). */
static int32_t civil_days(int y, int m, int d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (uint32_t)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (uint32_t)d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static uint32_t keep_crc(void)
{
    const uint8_t *p = (const uint8_t *)&s_keep;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(lp_keep_t, crc); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static bool keep_valid(void)
{
    return s_keep.magic == LP_KEEP_MAGIC && s_keep.size == sizeof(lp_keep_t) &&
           s_keep.crc == keep_crc() && s_keep.day != 0;
}
static void keep_reset(int32_t hday, uint8_t wday)
{
    memset(&s_keep, 0, sizeof(s_keep));
    s_keep.magic = LP_KEEP_MAGIC;
    s_keep.size  = sizeof(lp_keep_t);
    s_keep.day   = hday;
    s_keep.wday  = wday;
    s_keep.crc   = keep_crc();
}

/* Recompute the long-term averages (after a day was added, at boot). */
static void refresh_avg(void)
{
    static EXT_RAM_BSS_ATTR float a[2][LP_SLOTS];
    uint16_t nd[2] = { 0, 0 };
    load_hist_average(0, LH_AVG_WD, a[0], &nd[0]);
    load_hist_average(1, LH_AVG_WE, a[1], &nd[1]);
    int32_t oldest = 0;
    int cnt = load_hist_count(&oldest);
    portENTER_CRITICAL(&s_mux);
    memcpy(s_avg, a, sizeof(s_avg));
    s_avg_days[0] = nd[0]; s_avg_days[1] = nd[1];
    s_hist_count = cnt; s_hist_oldest = oldest;
    portEXIT_CRITICAL(&s_mux);
}

/* The measured day goes into the long-term record. Any slot measured counts:
 * the average works per quarter hour, a half day still says something. */
static void keep_flush(void)
{
    if (!keep_valid()) return;
    lh_day_t d;
    d.day = s_keep.day; d.wday = s_keep.wday;
    int n = 0;
    for (int i = 0; i < LP_SLOTS; i++) {
        if (s_keep.cnt[i]) {
            float w = s_keep.sum[i] / s_keep.cnt[i];
            d.w[i] = (uint16_t)lrintf(w < 0.0f ? 0.0f : (w > LP_MAX_W ? LP_MAX_W : w));
            n++;
        } else {
            d.w[i] = LH_NONE;
        }
    }
    if (n && s_hist_ok) {
        esp_err_t e = load_hist_put(&d);
        if (e == ESP_OK) {
            int y, m, dd;
            load_hist_date(d.day, &y, &m, &dd);
            ESP_LOGI(TAG, "Tag %02d.%02d.%04d im Langzeitspeicher (%d/96 Viertelstunden)", dd, m, y, n);
            refresh_avg();
        } else {
            ESP_LOGW(TAG, "Langzeitspeicher: Schreiben fehlgeschlagen (%s)", esp_err_to_name(e));
        }
    }
    s_keep.day = 0;
    s_keep.crc = keep_crc();
}

/* Local slot index, day type and day of a unix time. localtime_r follows the
 * TZ the firmware sets, so a DST change shifts the index by four slots:
 * a 25-hour day visits 02:00-02:59 twice, a 23-hour day never sees it. */
static lp_where_t where(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    lp_where_t w = {
        .idx  = (uint8_t)(tm.tm_hour * 4 + tm.tm_min / 15),
        .we   = (uint8_t)(tm.tm_wday == 0 || tm.tm_wday == 6),
        .wday = (uint8_t)tm.tm_wday,
        .day  = tm.tm_year * 400 + tm.tm_yday,
        .hday = civil_days(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday),
    };
    if (w.idx >= LP_SLOTS) w.idx = LP_SLOTS - 1;
    return w;
}

/* A day counts as learned when most of its quarter hours were. Distinct
 * slots, not updates: the 25-hour day would otherwise count 02:xx twice. */
static void close_day(void)
{
    if (s_day_key == 0) return;
    int n = 0;
    for (int i = 0; i < LP_SLOTS; i++) n += (s_day_seen[i / 8] >> (i % 8)) & 1;
    if (n >= LP_DAY_MIN) {
        portENTER_CRITICAL(&s_mux);
        if (s_days[s_day_we] < 0xFFFF) s_days[s_day_we]++;
        s_gen++;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "Tag gelernt (%s, %d/96 Viertelstunden)",
                 s_day_we ? "Wochenende" : "Werktag", n);
    }
    memset(s_day_seen, 0, sizeof(s_day_seen));
}

/* The slot being accumulated has ended: fold its mean into the profile if
 * samples covered enough of it. A slot with a long gap (Modbus down, a
 * restart) would be the mean of a few minutes -- a single kettle could make
 * it look like the household's habit. */
static void close_slot(void)
{
    if (s_slot_t == 0) return;
    if (s_cov >= (int32_t)(LP_COVER_MIN * EM_SLOT_S)) {
        float mean = (float)(s_sum / s_cov);
        portENTER_CRITICAL(&s_mux);
        float *p = &s_prof[s_slot_we][s_slot_idx];
        *p = isnan(*p) ? mean : *p + LP_ALPHA * (mean - *p);
        s_gen++;
        portEXIT_CRITICAL(&s_mux);
        if (s_slot_day == s_day_key) s_day_seen[s_slot_idx / 8] |= (uint8_t)(1u << (s_slot_idx % 8));
        if (keep_valid() && s_slot_hday == s_keep.day && s_keep.cnt[s_slot_idx] < 255) {
            s_keep.sum[s_slot_idx] += mean;
            s_keep.cnt[s_slot_idx]++;
            s_keep.crc = keep_crc();
        }
    }
    s_slot_t = 0;
}

static void start_slot(int64_t slot_t)
{
    lp_where_t w = where(slot_t);
    if (w.day != s_day_key) {
        close_day();
        s_day_key = w.day;
        s_day_we  = w.we;
    }
    /* The long-term day: carry on with today's if a restart left one, else
     * file whatever day is in there (the display was off at midnight, or
     * this is midnight) and start the new one. */
    if (!(keep_valid() && s_keep.day == w.hday)) {
        keep_flush();
        keep_reset(w.hday, w.wday);
    }
    s_slot_t   = slot_t;
    s_slot_idx = w.idx;
    s_slot_we  = w.we;
    s_slot_day = w.day;
    s_slot_hday = w.hday;
    s_sum = 0.0;
    s_cov = 0;
}

/* The house power if it can be trusted right now, else NAN. */
static float sample_w(void)
{
    modbus_tcp_status_t st;
    modbus_tcp_get_status(&st);
    if (st.dev_count == 0 || st.connected == 0 || !isfinite(st.house_w)) return NAN;
    float w = st.house_w;
    /* A negative house load is a measurement artefact (meters out of step),
     * not a house feeding the grid; above 30 kW is a misread. */
    return w < 0.0f ? 0.0f : (w > LP_MAX_W ? LP_MAX_W : w);
}

static void build_rec(lp_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->ver  = LP_VER;
    r->size = (uint16_t)sizeof(*r);
    portENTER_CRITICAL(&s_mux);
    r->days_wd = s_days[0];
    r->days_we = s_days[1];
    for (int d = 0; d < 2; d++)
        for (int i = 0; i < LP_SLOTS; i++) {
            float v = s_prof[d][i];
            r->w[d][i] = isnan(v) ? LP_NONE : (uint16_t)lrintf(v);
        }
    portEXIT_CRITICAL(&s_mux);
    r->sum = rec_sum(r);
}

/* Write if anything changed. NVS refuses the write while the partition is
 * nearly full (statistics must never crowd out settings): keep the change
 * and try again later, the profile in RAM stays valid meanwhile. */
static void save(int64_t now)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t gen = s_gen;
    portEXIT_CRITICAL(&s_mux);
    if (gen == s_saved_gen) return;

    lp_rec_t r;
    build_rec(&r);
    esp_err_t e = nvs_store_set_load_prof(&r, sizeof(r));
    if (e == ESP_OK) {
        s_saved_gen = gen;
        s_save_due  = now + LP_SAVE_EVERY_S;
        ESP_LOGI(TAG, "gespeichert (%u Werktage, %u Wochenendtage)",
                 (unsigned)r.days_wd, (unsigned)r.days_we);
    } else {
        s_save_due = now + LP_RETRY_S;
        ESP_LOGW(TAG, "Speichern abgelehnt (%s), neuer Versuch spaeter", esp_err_to_name(e));
    }
}

void load_profile_init(void)
{
    portENTER_CRITICAL(&s_mux);
    for (int d = 0; d < 2; d++)
        for (int i = 0; i < LP_SLOTS; i++) s_prof[d][i] = NAN;
    s_days[0] = s_days[1] = 0;
    s_gen = 0;
    portEXIT_CRITICAL(&s_mux);

    s_slot_t = 0; s_last_tick = 0; s_sum = 0.0; s_cov = 0;
    s_day_key = 0; s_day_we = 0;
    memset(s_day_seen, 0, sizeof(s_day_seen));
    s_save_due = 0; s_saved_gen = 0;

    portENTER_CRITICAL(&s_mux);
    for (int d = 0; d < 2; d++)
        for (int i = 0; i < LP_SLOTS; i++) s_avg[d][i] = NAN;
    s_avg_days[0] = s_avg_days[1] = 0;
    s_hist_count = 0; s_hist_oldest = 0;
    portEXIT_CRITICAL(&s_mux);
    s_hist_ok = load_hist_init() == ESP_OK;
    if (s_hist_ok) refresh_avg();
    else ESP_LOGW(TAG, "kein Langzeitspeicher -- Profil nur aus dem gleitenden Mittel");

    /* Zeroed first: a shorter stored record leaves the tail untouched, and
     * its `size` field then cannot match. */
    lp_rec_t r;
    memset(&r, 0, sizeof(r));
    esp_err_t e = nvs_store_get_load_prof(&r, sizeof(r));
    if (e != ESP_OK) {
        ESP_LOGI(TAG, "kein gespeichertes Lastprofil, beginne neu");
        return;
    }
    if (r.ver != LP_VER || r.size != sizeof(r) || r.sum != rec_sum(&r)) {
        ESP_LOGW(TAG, "Lastprofil fremder Version/Groesse oder beschaedigt (v%u, %u B) verworfen",
                 (unsigned)r.ver, (unsigned)r.size);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    for (int d = 0; d < 2; d++)
        for (int i = 0; i < LP_SLOTS; i++) {
            uint16_t v = r.w[d][i];
            s_prof[d][i] = (v == LP_NONE || v > (uint16_t)LP_MAX_W) ? NAN : (float)v;
        }
    s_days[0] = r.days_wd;
    s_days[1] = r.days_we;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Lastprofil geladen (%u Werktage, %u Wochenendtage)",
             (unsigned)r.days_wd, (unsigned)r.days_we);
}

void load_profile_tick(int64_t now)
{
    /* Without a set clock the slot index is fiction. Forget the half slot:
     * the next valid time may be hours away. */
    if (now < LP_CLOCK_MIN) {
        s_slot_t = 0;
        s_last_tick = 0;
        return;
    }
    /* The clock stepped back (NTP correction): the running slot cannot be
     * trusted, start over without learning it. */
    if (s_last_tick && now < s_last_tick) s_slot_t = 0;

    int64_t slot_t = em_slot_floor(now);
    if (slot_t != s_slot_t) {
        close_slot();
        start_slot(slot_t);
    }

    /* Each sample stands for the time since the previous tick, capped so a
     * stall of the task does not pass for coverage. The very first tick has
     * no previous one and carries no weight. */
    int64_t dt = s_last_tick && now > s_last_tick ? now - s_last_tick : 0;
    if (dt > LP_MAX_GAP_S) dt = LP_MAX_GAP_S;
    s_last_tick = now;

    float w = sample_w();
    if (!isnan(w) && dt > 0) {
        s_sum += (double)w * (double)dt;
        s_cov += (int32_t)dt;
    }

    if (s_save_due == 0) s_save_due = now + LP_SAVE_EVERY_S;   /* not right after boot */
    if (now >= s_save_due) save(now);
}

void load_profile_save_now(void)
{
    save(s_last_tick);
}

/* The profile the planner uses (call under s_mux): the long-term average
 * once it has LH_MIN_DAYS days of that type, the moving average before --
 * and for a slot the average has never seen. */
static float eff_w(int we, int i)
{
    if (s_avg_days[we] >= LH_MIN_DAYS && !isnan(s_avg[we][i])) return s_avg[we][i];
    return s_prof[we][i];
}

void load_profile_forecast(int64_t t0, uint16_t n, em_series_t *out)
{
    if (!out) return;
    if (n > EM_MAX_SLOTS) n = EM_MAX_SLOTS;
    t0 = em_slot_floor(t0);
    out->t0 = t0;
    out->n  = n;

    /* localtime_r may take a lock of its own: work out where each slot falls
     * first, then hold the spinlock only for the copy. */
    uint8_t at[EM_MAX_SLOTS];
    for (int i = 0; i < n; i++) {
        lp_where_t w = where(t0 + (int64_t)i * EM_SLOT_S);
        at[i] = (uint8_t)(w.idx | (w.we << 7));
    }
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        float v = eff_w(at[i] >> 7, at[i] & 0x7F);
        out->v[i] = isnan(v) ? NAN : v / 1000.0f;
    }
    portEXIT_CRITICAL(&s_mux);
    for (int i = n; i < EM_MAX_SLOTS; i++) out->v[i] = NAN;
}

void load_profile_get(load_profile_t *out)
{
    if (!out) return;
    bool kv = keep_valid();          /* tick-side data, read racily: display only */
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LP_SLOTS; i++) {
        float a = eff_w(0, i), b = eff_w(1, i);
        out->wd_kw[i] = isnan(a) ? NAN : a / 1000.0f;
        out->we_kw[i] = isnan(b) ? NAN : b / 1000.0f;
        out->today_kw[i] = kv && s_keep.cnt[i] ? s_keep.sum[i] / s_keep.cnt[i] / 1000.0f : NAN;
    }
    for (int d = 0; d < 2; d++) {
        out->src[d]     = s_avg_days[d] >= LH_MIN_DAYS;
        out->avg_days[d] = s_avg_days[d];
    }
    out->days_wd = s_days[0];
    out->days_we = s_days[1];
    out->hist_days   = (uint16_t)s_hist_count;
    out->hist_oldest = s_hist_oldest;
    out->hist_ok     = s_hist_ok;
    out->today_day   = kv ? s_keep.day : 0;
    portEXIT_CRITICAL(&s_mux);
}

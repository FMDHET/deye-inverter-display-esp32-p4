#include "harness.h"
#include "fakes.h"

#include <stdlib.h>
#include <time.h>

/* The load profile learns for weeks before anyone looks at it, and what it
 * learns steers when the battery is charged from the grid. A wrong slot
 * index, a day counted twice or a profile that silently stops being saved
 * would only show up as a bad plan much later -- so the clock is driven
 * here, through weekdays, weekends and both DST changes. */
#define LOAD_HIST_HOST_TEST
#include "load_hist.c"
#include "load_profile.c"

/* ------------- the long-term record's flash, in RAM ------------------- */
#define FLASH_MAX (64 * 1024)
static uint8_t f_flash[FLASH_MAX];
static lh_io_t s_ff_io;                           /* defined below */
static esp_err_t ff_read(size_t o, void *b, size_t l)
{
    if (o + l > s_ff_io.size) return ESP_ERR_INVALID_SIZE;
    memcpy(b, f_flash + o, l); return ESP_OK;
}
static esp_err_t ff_write(size_t o, const void *b, size_t l)
{
    if (o + l > s_ff_io.size) return ESP_ERR_INVALID_SIZE;
    const uint8_t *p = b;
    for (size_t i = 0; i < l; i++) f_flash[o + i] &= p[i];     /* NOR: bits only clear */
    return ESP_OK;
}
static esp_err_t ff_erase(size_t o, size_t l)
{
    if (o % LH_SECTOR || l % LH_SECTOR || o + l > s_ff_io.size) return ESP_ERR_INVALID_ARG;
    memset(f_flash + o, 0xFF, l); return ESP_OK;
}
static lh_io_t s_ff_io = { ff_read, ff_write, ff_erase, FLASH_MAX };

/* A new flash (erased) of `size` bytes. */
static void flash_blank(size_t size)
{
    memset(f_flash, 0xFF, sizeof(f_flash));
    s_ff_io.size = size;
    s_lh_io = &s_ff_io;
}

/* ------------------ the live balance the profile reads ------------------ */
static modbus_tcp_status_t f_st;
void modbus_tcp_get_status(modbus_tcp_status_t *o) { *o = f_st; }

#define TICK_S 10

/* Local time to unix, DST resolved by mktime (-1 = "you decide"). */
static int64_t lt(int y, int mo, int d, int h, int mi)
{
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

/* House power as a function of time. NAN = the Modbus link is down. */
typedef float (*load_fn)(int64_t t);

static float g_const;
static float f_const(int64_t t) { (void)t; return g_const; }

/* Tick every 10 s from `from` up to and including `to`. */
static void run(int64_t from, int64_t to, load_fn fn)
{
    for (int64_t t = from; t <= to; t += TICK_S) {
        float w = fn(t);
        f_st.connected = isnan(w) ? 0 : 1;
        f_st.house_w   = isnan(w) ? 0.0f : w;
        load_profile_tick(t);
    }
}

static void fresh(void)
{
    flash_blank(FLASH_MAX);
    memset(&s_keep, 0, sizeof(s_keep));           /* power cut: PSRAM gone */
    fake_nvs_reset();
    fake_nvs_reset_em();
    memset(&f_st, 0, sizeof(f_st));
    f_st.dev_count = 2;
    f_st.connected = 1;
    load_profile_init();
}

static load_profile_t prof(void)
{
    load_profile_t p;
    load_profile_get(&p);
    return p;
}

/* The fixed calendar of these tests: Mon 2026-10-19 .. Sun 2026-10-25, the
 * Sunday on which CEST ends. */
#define MON(h, m) lt(2026, 10, 19, h, m)
#define TUE(h, m) lt(2026, 10, 20, h, m)
#define WED(h, m) lt(2026, 10, 21, h, m)
#define THU(h, m) lt(2026, 10, 22, h, m)
#define FRI(h, m) lt(2026, 10, 23, h, m)
#define SAT(h, m) lt(2026, 10, 24, h, m)
#define SUN(h, m) lt(2026, 10, 25, h, m)

/* Learn one quarter hour at a constant load (closing tick included). */
static void learn_slot(int64_t t, float w)
{
    g_const = w;
    run(t, t + EM_SLOT_S, f_const);
}

/* ------------------------------------------------------------------------ */

static void test_the_calendar(void)
{
    struct tm tm;
    time_t t = (time_t)MON(12, 0);
    localtime_r(&t, &tm);
    CHECK_I(tm.tm_wday, 1);
    t = (time_t)SUN(12, 0);
    localtime_r(&t, &tm);
    CHECK_I(tm.tm_wday, 0);
    CHECK_I(MON(0, 0) % EM_SLOT_S, 0);
}

static int64_t g_half;
static float f_step(int64_t t) { return t < g_half ? 1000.0f : 2000.0f; }

static void test_the_slot_mean(void)
{
    fresh();
    int64_t t0 = MON(10, 0);
    g_half = t0 + EM_SLOT_S / 2;
    run(t0, t0 + EM_SLOT_S - TICK_S, f_step);
    /* Not before the slot is over: a half slot is not a habit. */
    CHECK(isnan(prof().wd_kw[40]));

    /* What the module should have averaged: every sample but the first
     * (which has no predecessor) stands for the 10 s before it. */
    double sum = 0; int n = 0;
    for (int64_t t = t0 + TICK_S; t < t0 + EM_SLOT_S; t += TICK_S) { sum += f_step(t); n++; }
    run(t0 + EM_SLOT_S, t0 + EM_SLOT_S, f_step);
    load_profile_t p = prof();
    CHECK_F(p.wd_kw[40], sum / n / 1000.0, 0.001);
    CHECK(isnan(p.wd_kw[39]));
    CHECK(isnan(p.wd_kw[41]));        /* the slot just begun is still open */
    CHECK(isnan(p.we_kw[40]));
}

static float f_dc_first_10min(int64_t t) { return (t - MON(10, 0)) < 600 ? NAN : 800.0f; }
static float f_dc_first_3min (int64_t t) { return (t - MON(11, 0)) < 180 ? NAN : 800.0f; }

static void test_the_coverage_rule(void)
{
    fresh();
    /* 10 of 15 minutes missing: 33 % covered. */
    run(MON(10, 0), MON(10, 15), f_dc_first_10min);
    CHECK(isnan(prof().wd_kw[40]));
    /* 3 of 15 minutes missing: 80 % covered -- learned. */
    run(MON(11, 0), MON(11, 15), f_dc_first_3min);
    CHECK_F(prof().wd_kw[44], 0.8, 0.001);
}

static int64_t g_t0;
static float f_two_thirds(int64_t t) { return ((t - g_t0) / TICK_S) % 3 == 0 ? NAN : 500.0f; }

static void test_bad_samples(void)
{
    fresh();
    learn_slot(MON(10, 0), 50000.0f);              /* a misread */
    CHECK_F(prof().wd_kw[40], 30.0, 0.001);
    learn_slot(MON(11, 0), -400.0f);               /* meters out of step */
    CHECK_F(prof().wd_kw[44], 0.0, 0.001);

    /* Every third sample non-finite: 67 % coverage, below the 70 %. */
    g_t0 = MON(12, 0);
    for (int64_t t = g_t0; t <= g_t0 + EM_SLOT_S; t += TICK_S) {
        f_st.connected = 1;
        f_st.house_w = isnan(f_two_thirds(t)) ? INFINITY : 500.0f;
        load_profile_tick(t);
    }
    CHECK(isnan(prof().wd_kw[48]));

    /* No device configured: nothing to learn from, however connected. */
    f_st.dev_count = 0;
    for (int64_t t = MON(13, 0); t <= MON(13, 15); t += TICK_S) {
        f_st.connected = 1; f_st.house_w = 700.0f;
        load_profile_tick(t);
    }
    CHECK(isnan(prof().wd_kw[52]));
}

static void test_the_clock_must_be_set(void)
{
    fresh();
    g_const = 900.0f;
    run(1000000000LL, 1000000000LL + 2 * EM_SLOT_S, f_const);   /* 2001: no NTP yet */
    load_profile_t p = prof();
    for (int i = 0; i < LP_SLOTS; i++) CHECK(isnan(p.wd_kw[i]) && isnan(p.we_kw[i]));
}

static void test_first_value_then_ema(void)
{
    fresh();
    learn_slot(MON(10, 0), 1000.0f);
    CHECK_F(prof().wd_kw[40], 1.0, 0.0005);       /* first value taken as is */
    learn_slot(TUE(10, 0), 2000.0f);
    CHECK_F(prof().wd_kw[40], 1.15, 0.0005);      /* 1000 + 0.15 * 1000 */
    learn_slot(WED(10, 0), 2000.0f);
    CHECK_F(prof().wd_kw[40], 1.2775, 0.0005);    /* 1150 + 0.15 * 850  */
}

static void test_weekday_and_weekend_apart(void)
{
    fresh();
    learn_slot(SAT(10, 0), 2000.0f);
    load_profile_t p = prof();
    CHECK_F(p.we_kw[40], 2.0, 0.0005);
    CHECK(isnan(p.wd_kw[40]));
    learn_slot(FRI(10, 0), 600.0f);               /* Friday is a weekday */
    learn_slot(SUN(10, 0), 1000.0f);              /* Sunday is weekend   */
    p = prof();
    CHECK_F(p.wd_kw[40], 0.6, 0.0005);
    CHECK_F(p.we_kw[40], 1.85, 0.0005);          /* 2000 + 0.15 * -1000 */
}

static float f_gap_6h(int64_t t) { return (t >= TUE(6, 0) && t < TUE(12, 0)) ? NAN : 400.0f; }

static void test_day_counting(void)
{
    fresh();
    g_const = 1000.0f;
    run(MON(0, 0), TUE(0, 0) - TICK_S, f_const);
    CHECK_I(prof().days_wd, 0);                   /* Monday not over yet */
    run(TUE(0, 0), TUE(0, 0), f_const);
    load_profile_t p = prof();
    CHECK_I(p.days_wd, 1);
    CHECK_I(p.days_we, 0);
    for (int i = 0; i < LP_SLOTS; i++) CHECK_F(p.wd_kw[i], 1.0, 0.0005);

    /* Tuesday with six hours missing: 72 of 96 slots, not a learned day --
     * but its slots still count. */
    run(TUE(0, 0) + TICK_S, WED(0, 0), f_gap_6h);
    p = prof();
    CHECK_I(p.days_wd, 1);
    /* Tuesday 00:00 holds the closing tick of Monday's run (1000 W) and
     * 89 samples at 400 W. */
    CHECK_F(p.wd_kw[0], (1000 + 0.15 * ((1000 + 89 * 400) / 90.0 - 1000)) / 1000, 0.0005);
    CHECK_F(p.wd_kw[30], 1.0, 0.0005);            /* 07:30 untouched    */

    /* A full Saturday counts for the weekend. */
    g_const = 1000.0f;
    run(SAT(0, 0), SUN(0, 0), f_const);
    p = prof();
    CHECK_I(p.days_wd, 1);
    CHECK_I(p.days_we, 1);
}

static void test_the_forecast(void)
{
    fresh();
    learn_slot(MON(10, 0), 1500.0f);
    learn_slot(SAT(10, 0), 2000.0f);

    em_series_t s;
    load_profile_forecast(FRI(9, 50), 3, &s);     /* not on the grid */
    CHECK_I(s.t0, FRI(9, 45));
    CHECK_I(s.n, 3);
    CHECK(isnan(s.v[0]));                         /* 09:45 never learned */
    CHECK_F(s.v[1], 1.5, 0.0005);                 /* weekday 10:00       */
    CHECK(isnan(s.v[2]));
    CHECK(isnan(s.v[3]));                         /* beyond n            */

    load_profile_forecast(FRI(10, 0), 500, &s);
    CHECK_I(s.n, EM_MAX_SLOTS);
    CHECK_F(s.v[0], 1.5, 0.0005);                 /* Friday 10:00   */
    CHECK_F(s.v[96], 2.0, 0.0005);                /* Saturday 10:00 */
    CHECK_F(em_series_at(&s, SAT(10, 7)), 2.0, 0.0005);
    int known = 0;
    for (int i = 0; i < EM_MAX_SLOTS; i++) known += !isnan(s.v[i]);
    CHECK_I(known, 2);

    /* Across the DST end: Sunday 10:00 local is 100 slots after Saturday
     * 10:00, and still the weekend 10:00 slot. */
    load_profile_forecast(SAT(10, 0), 101, &s);
    CHECK_F(s.v[100], 2.0, 0.0005);
    CHECK(isnan(s.v[96]));                        /* Sunday 09:00 */
}

static void test_persistence_round_trip(void)
{
    fresh();
    g_const = 1234.4f;
    run(MON(0, 0), TUE(0, 0), f_const);
    learn_slot(SAT(10, 0), 777.0f);
    load_profile_t before = prof();
    CHECK_I(before.days_wd, 1);

    load_profile_save_now();
    load_profile_init();                           /* a restart */
    load_profile_t after = prof();
    CHECK_I(after.days_wd, 1);
    CHECK_I(after.days_we, 0);
    for (int i = 0; i < LP_SLOTS; i++) CHECK_F(after.wd_kw[i], 1.234, 0.0005);
    CHECK_F(after.we_kw[40], 0.777, 0.0005);
    CHECK(isnan(after.we_kw[41]));

    /* Learning goes on from the stored value, not from scratch. */
    learn_slot(TUE(10, 0), 2234.0f);
    CHECK_F(prof().wd_kw[40], 1.384, 0.0005);     /* 1234 + 0.15 * 1000 */

    /* Nothing changed since the restart: save_now writes nothing. */
    fresh();
    load_profile_save_now();
    lp_rec_t r;
    CHECK_I(nvs_store_get_load_prof(&r, sizeof(r)), ESP_ERR_NOT_FOUND);
}

static void test_a_foreign_record_is_discarded(void)
{
    fresh();
    learn_slot(MON(10, 0), 1500.0f);
    load_profile_save_now();
    lp_rec_t good;
    CHECK_I(nvs_store_get_load_prof(&good, sizeof(good)), ESP_OK);

    lp_rec_t r = good;
    r.ver = 2;                                     /* a later firmware's layout */
    nvs_store_set_load_prof(&r, sizeof(r));
    load_profile_init();
    CHECK(isnan(prof().wd_kw[40]));

    r = good;
    r.size = (uint16_t)(sizeof(r) + 8);            /* longer than ours */
    r.sum  = rec_sum(&r);                          /* ... and consistent  */
    nvs_store_set_load_prof(&r, sizeof(r));
    load_profile_init();
    CHECK(isnan(prof().wd_kw[40]));

    r = good;
    r.w[0][40] = 900;                              /* a flipped bit */
    load_profile_init();
    CHECK(isnan(prof().wd_kw[40]));

    nvs_store_set_load_prof(&good, 100);           /* truncated */
    load_profile_init();
    CHECK(isnan(prof().wd_kw[40]));

    nvs_store_set_load_prof(&good, sizeof(good));  /* and the real one loads */
    load_profile_init();
    CHECK_F(prof().wd_kw[40], 1.5, 0.0005);
}

static uint16_t stored(int we, int idx)
{
    lp_rec_t r;
    memset(&r, 0, sizeof(r));
    if (nvs_store_get_load_prof(&r, sizeof(r)) != ESP_OK) return 0;
    return r.w[we][idx];
}

static void test_hourly_save_and_retry(void)
{
    fresh();
    g_const = 1000.0f;
    run(MON(10, 0), MON(10, 59) + 50, f_const);
    lp_rec_t r;
    CHECK_I(nvs_store_get_load_prof(&r, sizeof(r)), ESP_ERR_NOT_FOUND);   /* not in the first hour */

    run(MON(11, 0), MON(11, 0), f_const);         /* one hour after the first tick */
    CHECK_I(stored(0, 43), 1000);                 /* 10:45, closed by this tick */
    CHECK_I(stored(0, 44), LP_NONE);

    /* Within the hour: learned in RAM, not written. */
    run(MON(11, 0) + TICK_S, MON(11, 30), f_const);
    CHECK_F(prof().wd_kw[45], 1.0, 0.0005);
    CHECK_I(stored(0, 45), LP_NONE);

    /* The write is refused at 12:00 -- kept, tried again 10 minutes later. */
    fake_nvs_fail = true;
    run(MON(11, 30) + TICK_S, MON(12, 5), f_const);
    CHECK_I(stored(0, 46), LP_NONE);
    fake_nvs_fail = false;
    run(MON(12, 5) + TICK_S, MON(12, 10) - TICK_S, f_const);
    CHECK_I(stored(0, 46), LP_NONE);              /* not yet */
    run(MON(12, 10), MON(12, 10), f_const);
    CHECK_I(stored(0, 46), 1000);
    CHECK_I(stored(0, 47), 1000);                 /* 11:45, closed at 12:00 */
}

/* 25-hour day: 02:00-02:59 summer time, then again as winter time. */
static float f_fall(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    if (tm.tm_hour != 2) return 500.0f;
    return tm.tm_isdst > 0 ? 1000.0f : 3000.0f;
}

static void test_the_25_hour_day(void)
{
    fresh();
    int64_t from = SUN(0, 0), to = lt(2026, 10, 26, 0, 0);
    CHECK_I(to - from, 25 * 3600);
    run(from, to, f_fall);
    load_profile_t p = prof();
    CHECK_F(p.we_kw[8], 1.3, 0.0005);             /* 1000, then EMA towards 3000 */
    CHECK_F(p.we_kw[11], 1.3, 0.0005);
    CHECK_F(p.we_kw[0], 0.5, 0.0005);
    CHECK_F(p.we_kw[12], 0.5, 0.0005);
    CHECK_I(p.days_we, 1);                        /* once, not twice */
    CHECK(isnan(p.wd_kw[0]));
}

static void test_the_23_hour_day(void)
{
    fresh();
    int64_t from = lt(2026, 3, 29, 0, 0), to = lt(2026, 3, 30, 0, 0);
    CHECK_I(to - from, 23 * 3600);
    g_const = 600.0f;
    run(from, to, f_const);
    load_profile_t p = prof();
    for (int i = 8; i < 12; i++) CHECK(isnan(p.we_kw[i]));   /* 02:xx never happened */
    CHECK_F(p.we_kw[7], 0.6, 0.0005);
    CHECK_F(p.we_kw[12], 0.6, 0.0005);
    CHECK_I(p.days_we, 1);                        /* 92 slots are enough */
}

static void test_a_clock_step_back(void)
{
    fresh();
    g_const = 1000.0f;
    run(MON(10, 0), MON(10, 10), f_const);
    /* NTP pulls the clock back by an hour: the open slot is dropped, the
     * slot of the new time starts clean. */
    g_const = 3000.0f;
    run(MON(9, 10), MON(9, 15), f_const);
    CHECK(isnan(prof().wd_kw[40]));
    CHECK(isnan(prof().wd_kw[36]));               /* 09:00, only 5 min covered */
}

/* --------------------------- the long-term record ----------------------- */

/* A different load in every quarter hour, so a shifted slot shows. */
static float f_pattern(int64_t t)
{
    lp_where_t w = where(t);
    return 300.0f + 10.0f * w.idx + (w.we ? 500.0f : 0.0f);
}
static float g_scale = 1.0f;
static float f_pattern_scaled(int64_t t) { return f_pattern(t) * g_scale; }

static void test_days_go_into_the_record(void)
{
    fresh();
    run(MON(0, 0), THU(0, 20), f_pattern);        /* Mon, Tue, Wed complete */
    int32_t oldest = 0;
    CHECK_I(load_hist_count(&oldest), 3);
    int y, m, d;
    load_hist_date(oldest, &y, &m, &d);
    CHECK_I(y, 2026); CHECK_I(m, 10); CHECK_I(d, 19);
    lh_day_t day;
    CHECK(load_hist_get(0, &day));                /* the newest: Wednesday */
    load_hist_date(day.day, &y, &m, &d);
    CHECK_I(d, 21);
    CHECK_I(day.wday, 3);
    for (int i = 0; i < LH_SLOTS; i++) CHECK_I(day.w[i], 300 + 10 * i);

    /* Three weekdays: the profile is now their average, not the EMA. */
    load_profile_t p = prof();
    CHECK_I(p.src[0], 1);
    CHECK_I(p.avg_days[0], 3);
    CHECK_I(p.src[1], 0);                         /* no weekend day yet */
    CHECK_I(p.hist_days, 3);
    CHECK(p.hist_ok);
    for (int i = 0; i < LP_SLOTS; i++) CHECK_F(p.wd_kw[i], (300 + 10 * i) / 1000.0, 0.0006);
    /* today (Thursday) so far: the first slot */
    CHECK_F(p.today_kw[0], 0.300, 0.0006);
    CHECK(isnan(p.today_kw[1]));
}

/* An odd day (a party, a week away) does not bend the profile: trimmed mean. */
static void test_the_average_resists_outliers(void)
{
    fresh();
    run(MON(0, 0), SAT(0, 5), f_pattern);         /* five normal weekdays */
    g_scale = 10.0f;                              /* one wild weekday...  */
    run(lt(2026, 10, 26, 0, 0), lt(2026, 10, 27, 0, 5), f_pattern_scaled);
    g_scale = 1.0f;
    load_profile_t p = prof();
    CHECK_I(p.avg_days[0], 6);
    /* 6 values per slot -> cut 1 at each end: the wild day drops out */
    for (int i = 0; i < LP_SLOTS; i++) CHECK_F(p.wd_kw[i], (300 + 10 * i) / 1000.0, 0.0006);
}

/* An OTA in the middle of the day: the day so far is kept (PSRAM that a
 * software restart does not clear), the record gets the whole day. A power
 * cut loses the morning -- the record then holds the afternoon only. */
static void test_a_restart_keeps_the_day(void)
{
    fresh();
    run(MON(0, 0), MON(12, 0), f_pattern);
    load_profile_save_now();
    load_profile_init();                          /* software restart */
    run(MON(12, 0), TUE(0, 5), f_pattern);
    lh_day_t day;
    CHECK(load_hist_get(0, &day));
    int known = 0;
    for (int i = 0; i < LH_SLOTS; i++) known += day.w[i] != LH_NONE;
    CHECK(known >= 95);                           /* the restart's own slot may miss */

    fresh();
    run(MON(0, 0), MON(12, 0), f_pattern);
    memset(&s_keep, 0, sizeof(s_keep));           /* power cut */
    load_profile_init();
    run(MON(12, 0), TUE(0, 5), f_pattern);
    CHECK(load_hist_get(0, &day));
    known = 0;
    for (int i = 0; i < LH_SLOTS; i++) known += day.w[i] != LH_NONE;
    CHECK(known <= 49);
    CHECK_I(day.w[10], LH_NONE);                  /* the morning is gone */
    CHECK_I(day.w[60], 300 + 10 * 60);

    /* Off over midnight: the day in PSRAM is filed when the display comes
     * back the next morning, not lost. */
    fresh();
    run(MON(0, 0), MON(23, 0), f_pattern);
    load_profile_init();
    run(TUE(8, 0), TUE(8, 30), f_pattern);
    CHECK_I(load_hist_count(NULL), 1);
    CHECK(load_hist_get(0, &day));
    CHECK_I(day.wday, 1);                         /* Monday */
    CHECK_I(day.w[0], 300);

    /* A damaged copy in PSRAM is not filed. */
    fresh();
    run(MON(0, 0), MON(23, 0), f_pattern);
    s_keep.sum[3] += 1.0f;                        /* checksum no longer fits */
    load_profile_init();
    run(TUE(8, 0), TUE(8, 30), f_pattern);
    CHECK_I(load_hist_count(NULL), 0);
}

static void test_the_ring(void)
{
    /* 8 sectors = 128 days of room; 200 days written: the ring wraps, the
     * oldest sectors go 16 days at a time. */
    flash_blank(8 * LH_SECTOR);
    CHECK_I(load_hist_init(), ESP_OK);
    lh_day_t d;
    for (int i = 0; i < LH_SLOTS; i++) d.w[i] = (uint16_t)i;
    for (int k = 0; k < 200; k++) {
        d.day = 20000 + k; d.wday = (uint8_t)(k % 7);
        CHECK_I(load_hist_put(&d), ESP_OK);
    }
    int32_t oldest;
    int n = load_hist_count(&oldest);
    CHECK(n >= 112 && n <= 128);
    CHECK_I(oldest, 20000 + 200 - n);
    lh_day_t g;
    CHECK(load_hist_get(0, &g));
    CHECK_I(g.day, 20199);
    CHECK(load_hist_get(n - 1, &g));
    CHECK_I(g.day, oldest);
    CHECK(!load_hist_get(n, &g));

    /* A restart finds the same: newest by sequence, not by position. */
    CHECK_I(load_hist_init(), ESP_OK);
    CHECK_I(load_hist_count(&oldest), n);
    CHECK(load_hist_get(0, &g));
    CHECK_I(g.day, 20199);

    /* A record torn by a power loss mid-write is skipped. */
    size_t slot = (size_t)s_lh_newest * LH_REC_SIZE;
    f_flash[slot + 40] = 0x00;
    CHECK_I(load_hist_init(), ESP_OK);
    CHECK_I(load_hist_count(NULL), n - 1);
    CHECK(load_hist_get(0, &g));
    CHECK_I(g.day, 20198);
    /* ... and the next write goes after it, not over it. */
    d.day = 30000;
    CHECK_I(load_hist_put(&d), ESP_OK);
    CHECK(load_hist_get(0, &g));
    CHECK_I(g.day, 30000);
    CHECK(load_hist_get(1, &g));
    CHECK_I(g.day, 20198);

    /* Day keys back to dates, across a leap day. */
    int y, m, dd;
    load_hist_date(civil_days(2028, 2, 29), &y, &m, &dd);
    CHECK_I(y, 2028); CHECK_I(m, 2); CHECK_I(dd, 29);
    CHECK_I(civil_days(1970, 1, 1), 0);
    CHECK_I(civil_days(2026, 10, 22), 20748);
}

/* No flash area at all: the profile works as before, from the EMA. */
static void test_without_the_record(void)
{
    fresh();
    s_lh_io = NULL;
    load_profile_init();
    run(MON(0, 0), THU(0, 5), f_pattern);
    load_profile_t p = prof();
    CHECK(!p.hist_ok);
    CHECK_I(p.src[0], 0);
    CHECK_F(p.wd_kw[10], 0.400, 0.0006);
}

int main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    RUN(test_days_go_into_the_record);
    RUN(test_the_average_resists_outliers);
    RUN(test_a_restart_keeps_the_day);
    RUN(test_the_ring);
    RUN(test_without_the_record);
    RUN(test_the_calendar);
    RUN(test_the_slot_mean);
    RUN(test_the_coverage_rule);
    RUN(test_bad_samples);
    RUN(test_the_clock_must_be_set);
    RUN(test_first_value_then_ema);
    RUN(test_weekday_and_weekend_apart);
    RUN(test_day_counting);
    RUN(test_the_forecast);
    RUN(test_persistence_round_trip);
    RUN(test_a_foreign_record_is_discarded);
    RUN(test_hourly_save_and_retry);
    RUN(test_the_25_hour_day);
    RUN(test_the_23_hour_day);
    RUN(test_a_clock_step_back);
    return t_report("load_profile");
}

#include "harness.h"
#include "fakes.h"

/* The locks must hold through a restart (an update while the car charges),
 * end by themselves when their time is up, and never stay on by accident:
 * every path to "off" lifts them. The meter emulation and the Fronius module
 * are faked: what matters here is what bat_lock asks of them. */
#include "bat_lock.c"

static uint32_t f_lock_s;                /* last modbus_rtu_set_lock()        */
static int      f_lock_calls;
void modbus_rtu_set_lock(uint32_t for_s) { f_lock_s = for_s; f_lock_calls++; }
static bool     f_byd;                   /* last fronius_storage_lock_discharge() */
void fronius_storage_lock_discharge(bool on) { f_byd = on; }

#define T0 1792700000LL

static void fresh(void)
{
    fake_nvs_reset_em();
    f_lock_s = 0; f_lock_calls = 0; f_byd = false;
    bat_lock_init();
}

static bat_lock_t get(bat_t w) { bat_lock_t l; bat_lock_get(w, &l); return l; }

static void test_deye_until_lifted(void)
{
    fresh();
    CHECK(!get(BAT_DEYE).on);
    CHECK_I(f_lock_calls, 0);
    CHECK_I(bat_lock_set(BAT_DEYE, true, 0, T0), ESP_OK);
    CHECK(get(BAT_DEYE).on);
    CHECK_I(get(BAT_DEYE).until, 0);
    CHECK_I(f_lock_s, BAT_LOCK_RENEW_S);        /* at once, not at the next tick */
    for (int64_t t = T0; t < T0 + 3 * 86400; t += 10) bat_lock_tick(t, true);
    CHECK(get(BAT_DEYE).on);
    CHECK(!f_byd);                               /* the other battery untouched */
    CHECK_I(bat_lock_set(BAT_DEYE, false, 0, T0 + 3 * 86400), ESP_OK);
    CHECK_I(f_lock_s, 0);
    int n = f_lock_calls;
    bat_lock_tick(T0 + 3 * 86400 + 10, true);
    CHECK_I(f_lock_calls, n);                    /* off: no renewing */
}

static void test_byd_timed(void)
{
    fresh();
    CHECK_I(bat_lock_set(BAT_BYD, true, 120, T0), ESP_OK);
    bat_lock_tick(T0, true);
    CHECK(f_byd);
    CHECK_I(f_lock_calls, 0);                    /* the Deye untouched */
    bat_lock_tick(T0 + 120 * 60 - 1, true);
    CHECK(f_byd);
    bat_lock_tick(T0 + 120 * 60, true);
    CHECK(!f_byd);
    CHECK(!get(BAT_BYD).on);

    /* BYD control not allowed: not passed on, still shown as asked for */
    bat_lock_set(BAT_BYD, true, 0, T0);
    bat_lock_tick(T0 + 10, false);
    CHECK(!f_byd);
    CHECK(get(BAT_BYD).on);
    bat_lock_tick(T0 + 20, true);
    CHECK(f_byd);
    /* lifted: at once, not only at the next tick */
    bat_lock_set(BAT_BYD, false, 0, T0 + 30);
    CHECK(!f_byd);
}

static void test_limits(void)
{
    fresh();
    CHECK_I(bat_lock_set(BAT_DEYE, true, BAT_LOCK_MAX_MIN + 1, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(bat_lock_set(BAT_BYD, true, 30, 1000), ESP_ERR_INVALID_STATE);   /* clock not set */
    CHECK_I(bat_lock_set((bat_t)5, true, 0, T0), ESP_ERR_INVALID_ARG);
    CHECK(!get(BAT_DEYE).on && !get(BAT_BYD).on);
    CHECK_I(bat_lock_set(BAT_BYD, true, 0, 1000), ESP_OK);                  /* until lifted: no clock needed */
}

static void test_restart(void)
{
    /* Both kept over a restart; the Deye holds again right away. */
    fresh();
    bat_lock_set(BAT_DEYE, true, 0, T0);
    bat_lock_set(BAT_BYD, true, 60, T0);
    f_lock_s = 0;
    bat_lock_init();
    CHECK(get(BAT_DEYE).on && get(BAT_BYD).on);
    CHECK_I(get(BAT_BYD).until, T0 + 3600);
    CHECK_I(f_lock_s, BAT_LOCK_RENEW_S);
    bat_lock_tick(5, true);                      /* clock not set yet: stays on */
    CHECK(get(BAT_BYD).on && f_byd);
    bat_lock_tick(T0 + 3600, true);
    CHECK(!get(BAT_BYD).on && !f_byd);
    CHECK(get(BAT_DEYE).on);

    /* The record of build 358 (Deye alone) is taken over. */
    fresh();
    struct { uint16_t ver; uint8_t on, r; int64_t until; } v1 = { 1, 1, 0, 0 };
    nvs_store_set_dlock(&v1, sizeof(v1));
    bat_lock_init();
    CHECK(get(BAT_DEYE).on);
    CHECK_I(get(BAT_DEYE).until, 0);
    CHECK(!get(BAT_BYD).on);

    /* A record of an unknown version is not trusted. */
    fresh();
    struct { uint16_t ver; uint8_t on[2], r[2]; int64_t until[2]; } bad = { 9, { 1, 1 }, { 0, 0 }, { 0, 0 } };
    nvs_store_set_dlock(&bad, sizeof(bad));
    bat_lock_init();
    CHECK(!get(BAT_DEYE).on && !get(BAT_BYD).on);
}

int main(void)
{
    RUN(test_deye_until_lifted);
    RUN(test_byd_timed);
    RUN(test_limits);
    RUN(test_restart);
    return t_report("bat_lock");
}

#include "harness.h"

/* Lifetime counters for Home Assistant. The numbers are the real ones of
 * 2026-10-25: Eltako 185.64 / 1087.52 kWh (0.01 kWh registers), Deye battery
 * 2900.6 / 2748.8 kWh (0.1 kWh registers). */
#include "energy_ctr.h"

static void test_normal_steps(void)
{
    energy_ctr_t c = {0};
    CHECK(energy_ctr_feed(&c, 18564, 1000, 100.0));          /* first value: taken */
    CHECK(energy_ctr_feed(&c, 18564, 11000, 100.0));         /* standing still     */
    CHECK(energy_ctr_feed(&c, 18571, 21000, 100.0));         /* +0.07 kWh in 10 s  */
    CHECK_I(c.val, 18571);
    CHECK_I(c.rejects, 0);
}

static void test_backwards_and_spikes(void)
{
    energy_ctr_t c = {0};
    energy_ctr_feed(&c, 29006, 0, 10.0);
    CHECK(!energy_ctr_feed(&c, 29005, 30000, 10.0));          /* runs backwards     */
    CHECK_I(c.val, 29006);
    /* 100 kW for 30 s is 0.83 kWh, plus 0.1 slack: 9 units of 0.1 kWh pass,
     * a garbled high word never does. */
    CHECK(energy_ctr_feed(&c, 29015, 30000, 10.0));
    CHECK(!energy_ctr_feed(&c, 29015 + 65536, 60000, 10.0));
    CHECK_I(c.val, 29015);
    /* A long silence allows a correspondingly larger step. */
    CHECK(energy_ctr_feed(&c, 29015 + 500, 60000 + 3600000, 10.0));
}

static void test_reset_taken_after_three(void)
{
    energy_ctr_t c = {0};
    energy_ctr_feed(&c, 18564, 0, 100.0);
    CHECK(!energy_ctr_feed(&c, 12, 10000, 100.0));            /* meter swapped ...  */
    CHECK(!energy_ctr_feed(&c, 12, 20000, 100.0));
    CHECK(energy_ctr_feed(&c, 13, 30000, 100.0));             /* ... third time: real */
    CHECK_I(c.val, 13);
    CHECK_I(c.rejects, 0);
    /* A single good value in between resets the count. */
    CHECK(!energy_ctr_feed(&c, 0, 40000, 100.0));
    CHECK(energy_ctr_feed(&c, 13, 50000, 100.0));
    CHECK(!energy_ctr_feed(&c, 0, 60000, 100.0));
    CHECK_I(c.rejects, 1);
}

static void test_ms_wraparound(void)
{
    energy_ctr_t c = {0};
    energy_ctr_feed(&c, 100, 0xFFFFF000u, 100.0);
    CHECK(energy_ctr_feed(&c, 101, 0x00001000u, 100.0));      /* 8 s later, across the wrap */
}

static void test_integrate(void)
{
    double chg = 0, dis = 0;
    energy_integrate(&chg, &dis, 2000.0f, 3600000);           /* 2 kW out for 1 h   */
    CHECK_F(dis, 2000.0, 1e-6);
    CHECK_F(chg, 0.0, 1e-9);
    energy_integrate(&chg, &dis, -1500.0f, 2000);             /* charging, 2 s      */
    CHECK_F(chg, 1500.0 * 2 / 3600.0, 1e-9);
    CHECK_F(dis, 2000.0, 1e-6);                               /* never decreases    */
    energy_integrate(&chg, &dis, 0.0f, 2000);
    CHECK_F(chg, 1500.0 * 2 / 3600.0, 1e-9);
}

int main(void)
{
    RUN(test_normal_steps);
    RUN(test_backwards_and_spikes);
    RUN(test_reset_taken_after_three);
    RUN(test_ms_wraparound);
    RUN(test_integrate);
    return t_report("energy_ctr");
}

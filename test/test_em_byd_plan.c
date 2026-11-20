#include "harness.h"

/* The BYD keeps its energy for the dearest hours before the next sun. */
#include "em_byd_plan.h"

static float pr[96], pv[96], ld[96];
static uint8_t blk[96];

static void evening(void)
{
    /* 16:00 .. 16:00 next day in quarter hours: afternoon 25 ct, evening
     * 17-21 h 45 ct, night 20 ct, sun from 9 h (slot 68) */
    for (int i = 0; i < 96; i++) {
        pr[i] = 25; pv[i] = 0; ld[i] = 0.8f;
        if (i >= 4 && i < 20) pr[i] = 45;
        if (i >= 20) pr[i] = 20;
        if (i >= 68) { pv[i] = 4.0f; pr[i] = 18; }
    }
}

static void test_keeps_it_for_the_evening(void)
{
    evening();
    /* 2 kWh: 0.8 kW x 0.25 h = 0.2 kWh per slot -> 10 evening slots */
    float p = em_byd_plan(pr, pv, ld, 96, 2.0f, 2.0f, blk);
    CHECK_F(p, 45, 0);
    int blocked_af = 0, blocked_ev = 0, blocked_night = 0, blocked_sun = 0;
    for (int i = 0; i < 4; i++) blocked_af += blk[i];
    for (int i = 4; i < 20; i++) blocked_ev += blk[i];
    for (int i = 20; i < 68; i++) blocked_night += blk[i];
    for (int i = 68; i < 96; i++) blocked_sun += blk[i];
    CHECK_I(blocked_af, 4);            /* the cheap afternoon waits          */
    CHECK_I(blocked_ev, 6);            /* 10 of 16 evening slots are served  */
    CHECK_I(blocked_night, 48);
    CHECK_I(blocked_sun, 0);           /* past the refill nothing is blocked */
}

static void test_enough_energy_blocks_nothing(void)
{
    evening();
    float p = em_byd_plan(pr, pv, ld, 96, 50.0f, 2.0f, blk);
    CHECK(isnan(p));
    int b = 0;
    for (int i = 0; i < 96; i++) b += blk[i];
    CHECK_I(b, 0);
}

static void test_no_energy_blocks_nothing(void)
{
    evening();
    CHECK(isnan(em_byd_plan(pr, pv, ld, 96, 0.0f, 2.0f, blk)));
    int b = 0;
    for (int i = 0; i < 96; i++) b += blk[i];
    CHECK_I(b, 0);
}

static void test_surplus_slots_never_blocked(void)
{
    evening();
    for (int i = 0; i < 2; i++) pv[i] = 1.0f;     /* 0.2 kW surplus, not a refill */
    em_byd_plan(pr, pv, ld, 96, 1.0f, 2.0f, blk);
    CHECK_I(blk[0], 0);
    CHECK_I(blk[1], 0);
    CHECK_I(blk[2], 1);
}

int main(void)
{
    RUN(test_keeps_it_for_the_evening);
    RUN(test_enough_energy_blocks_nothing);
    RUN(test_no_energy_blocks_nothing);
    RUN(test_surplus_slots_never_blocked);
    return t_report("em_byd_plan");
}

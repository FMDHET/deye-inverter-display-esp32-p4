#include "harness.h"

/* The SLS export guard's decision. It lowers the power an inverter feeds into
 * the grid so the house connection fuse is not overloaded -- the one piece of
 * this firmware whose failure mode is a fuse, not a wrong number on a screen.
 *
 * Every case here used to need a real forced discharge against a real meter:
 * "export has had headroom for a full minute", "the first correction was not
 * enough", "the operator lowered the slider while throttled". With the clock
 * in hand they cost microseconds. */
#include "sls_guard.h"

/* 35 A fuse: 35 x 3 x 230 x 0.9 = 21735 W. The number the device runs on. */
#define MAX35   (35.0f * 3.0f * 230.0f * 0.9f)
#define PMIN    1000

static sls_guard_state_t st;
static uint32_t          t;

static void reset_all(void)
{
    memset(&st, 0, sizeof(st));
    t = 100000;                 /* not 0: catches "never written" mix-ups */
}

/* Run one tick and advance the clock by the aggregator's cadence (800 ms). */
static sls_decision_t tick(float export_w, int applied, int user)
{
    sls_decision_t d = sls_guard_decide(export_w, MAX35, applied, user,
                                        t, PMIN, &st);
    t += 800;
    return d;
}

/* ------------------------- throttling down ----------------------------- */

static void test_within_limit_writes_nothing(void)
{
    reset_all();
    sls_decision_t d = tick(10000.0f, 10000, 10000);
    CHECK(!d.write);                       /* far below the fuse: no traffic */
    CHECK_I(d.target_w, 10000);
}

static void test_over_the_limit_throttles(void)
{
    reset_all();
    /* 23000 W out, 21735 allowed -> 1265 over. 10000 - 1265 = 8735,
     * rounded down to the 500 W step = 8500. */
    sls_decision_t d = tick(23000.0f, 10000, 10000);
    CHECK(d.write);
    CHECK_I(d.target_w, 8500);
    CHECK(!d.urgent);                      /* 1265 W is under the urgent mark */
}

static void test_correction_is_measured_from_the_applied_power(void)
{
    reset_all();
    /* THE bug this rewrite exists for. Already throttled to 9000 and still
     * 500 W over the limit. Measured against the USER setpoint the target
     * came out at 10000 - 500 = 9500 -- MORE power while the fuse is over
     * its rating. From the applied value it goes down, as it must. */
    sls_decision_t d = tick(MAX35 + 500.0f, 9000, 10000);
    CHECK(d.write);
    CHECK_I(d.target_w, 8500);
    CHECK(d.target_w < 9000);
}

static void test_throttle_converges_instead_of_oscillating(void)
{
    reset_all();
    /* The old guard restored to the user setpoint as soon as the export was
     * back under the limit -- which it only was BECAUSE of the throttle -- so
     * it throttled and restored forever, two EEPROM writes per 1.6 s.
     *
     * Model that exact feedback: export follows the applied power. At 10000 W
     * applied the house exports 23000 W; every watt of throttle is a watt
     * less export. Count the writes over five simulated minutes. */
    int   applied = 10000;
    int   writes  = 0;
    for (int i = 0; i < 375; i++) {           /* 375 x 800 ms = 5 min */
        float export_w = 23000.0f - (float)(10000 - applied);
        sls_decision_t d = tick(export_w, applied, 10000);
        if (d.write) { applied = d.target_w; writes++; }
    }
    /* It must settle, not hunt. The old code wrote on almost every tick. */
    CHECK(writes < 10);
    /* And it must settle at a power whose export actually fits the fuse. */
    float settled = 23000.0f - (float)(10000 - applied);
    CHECK(settled <= MAX35);
}

static void test_a_large_excess_ignores_the_rate_limit(void)
{
    reset_all();
    sls_decision_t d = tick(23000.0f, 10000, 10000);   /* first write */
    CHECK(d.write);
    int applied = d.target_w;

    /* Immediately afterwards, a big new excursion. The rate limit must not
     * hold the protective direction back. */
    d = tick(MAX35 + 5000.0f, applied, 10000);
    CHECK(d.write);
    CHECK(d.urgent);
    CHECK(d.target_w < applied);
}

static void test_a_small_excess_waits_for_the_rate_limit(void)
{
    reset_all();
    sls_decision_t d = tick(23000.0f, 10000, 10000);
    CHECK(d.write);
    int applied = d.target_w;

    /* Still a little over, but not urgent, and only 800 ms later. */
    d = tick(MAX35 + 600.0f, applied, 10000);
    CHECK(!d.write);

    /* Past the gap it goes through. */
    t += SLS_MIN_WRITE_MS;
    d = tick(MAX35 + 600.0f, applied, 10000);
    CHECK(d.write);
    CHECK(d.target_w < applied);
}

static void test_throttle_never_goes_below_the_inverter_minimum(void)
{
    reset_all();
    /* Absurd excess: the target would be far negative. */
    sls_decision_t d = tick(MAX35 + 90000.0f, 5000, 10000);
    CHECK(d.write);
    CHECK_I(d.target_w, PMIN);
}

/* -------------------------- giving power back -------------------------- */

static void test_restore_needs_headroom_not_just_being_under(void)
{
    reset_all();
    st.written = true;              /* pretend the guard has throttled before */
    /* 500 W under the limit is inside the hysteresis band: that is where the
     * old code restored to full power and started the oscillation. */
    int writes = 0;
    for (int i = 0; i < 200; i++)
        if (tick(MAX35 - 500.0f, 8500, 10000).write) writes++;
    CHECK_I(writes, 0);
}

static void test_restore_needs_the_headroom_to_last(void)
{
    reset_all();
    /* Real headroom, but only for a moment -- 30 s is not the full minute. */
    int writes = 0;
    for (int i = 0; i < 37; i++)                   /* 37 x 800 ms = 29.6 s */
        if (tick(MAX35 - 5000.0f, 8500, 10000).write) writes++;
    CHECK_I(writes, 0);
}

static void test_restore_steps_up_once_the_hold_has_passed(void)
{
    reset_all();
    sls_decision_t d = tick(MAX35 - 5000.0f, 8500, 10000);
    CHECK(!d.write);                               /* hold starts here */

    t += (uint32_t)SLS_RESTORE_HOLD_S * 1000u;
    d = tick(MAX35 - 5000.0f, 8500, 10000);
    CHECK(d.write);
    CHECK_I(d.target_w, 8500 + SLS_STEP_W);        /* one step, not a jump */
    CHECK(d.excess_w == 0.0f);
}

static void test_restore_walks_up_one_step_per_hold(void)
{
    reset_all();
    int applied = 8500;
    for (int step = 0; step < 3; step++) {
        sls_decision_t d = tick(MAX35 - 5000.0f, applied, 10000);
        CHECK(!d.write);                           /* hold restarts each time */
        t += (uint32_t)SLS_RESTORE_HOLD_S * 1000u;
        d = tick(MAX35 - 5000.0f, applied, 10000);
        CHECK(d.write);
        CHECK_I(d.target_w, applied + SLS_STEP_W);
        applied = d.target_w;
    }
    CHECK_I(applied, 10000);
}

static void test_restore_stops_at_the_user_setpoint(void)
{
    reset_all();
    /* Already at what the user asked for: nothing to give back, however much
     * headroom there is. */
    int writes = 0;
    for (int i = 0; i < 300; i++)
        if (tick(0.0f, 10000, 10000).write) writes++;
    CHECK_I(writes, 0);
}

static void test_restore_lands_exactly_on_an_odd_user_setpoint(void)
{
    reset_all();
    /* 5300 W is not a multiple of the step. Rounding down alone would stop at
     * 5000 and never reach it; the cap has to be applied after the rounding. */
    sls_decision_t d = tick(0.0f, 5000, 5300);
    CHECK(!d.write);
    t += (uint32_t)SLS_RESTORE_HOLD_S * 1000u;
    d = tick(0.0f, 5000, 5300);
    CHECK(d.write);
    CHECK_I(d.target_w, 5300);
}

static void test_an_excess_cancels_a_running_hold(void)
{
    reset_all();
    sls_decision_t d = tick(MAX35 - 5000.0f, 8500, 10000);   /* hold starts */
    CHECK(!d.write);

    t += 50u * 1000u;                                        /* 50 s of it */
    d = tick(MAX35 + 3000.0f, 8500, 10000);                  /* over again */
    CHECK(d.write);
    int applied = d.target_w;

    /* The hold must start from scratch -- the remaining 10 s must not let it
     * hand power back seconds after an excursion. */
    t += 15u * 1000u;
    d = tick(MAX35 - 5000.0f, applied, 10000);
    CHECK(!d.write);
}

/* ------------------------- operator interaction ------------------------ */

static void test_lowering_the_setpoint_below_the_throttle_is_respected(void)
{
    reset_all();
    /* Throttled to 8500, then the operator drags the slider down to 3000.
     * The guard must never push the power back above what the user asked. */
    t += (uint32_t)SLS_RESTORE_HOLD_S * 1000u;
    int writes = 0;
    for (int i = 0; i < 200; i++)
        if (tick(0.0f, 3000, 3000).write) writes++;
    CHECK_I(writes, 0);
}

static void test_the_decision_leaves_the_user_setpoint_alone(void)
{
    reset_all();
    /* The guard reports a target; it has no way to express a change to the
     * user setpoint, which is exactly the property that keeps the Home
     * Assistant slider from jumping on its own. Guarded by construction --
     * asserted here so a future signature change has to think about it. */
    int user = 10000;
    sls_decision_t d = tick(23000.0f, 10000, user);
    CHECK(d.write);
    CHECK_I(user, 10000);
    CHECK(d.target_w < user);
}

/* ------------------------------ edges ---------------------------------- */

static void test_the_clock_may_wrap(void)
{
    reset_all();
    /* uptime ms in a uint32 wraps after 49 days. The arithmetic is unsigned
     * differences throughout; a hold that straddles the wrap must still end
     * after its minute, not after another 49 days. */
    t = 0xFFFFFF00u;
    sls_decision_t d = tick(MAX35 - 5000.0f, 8500, 10000);
    CHECK(!d.write);
    t += (uint32_t)SLS_RESTORE_HOLD_S * 1000u;    /* wraps past zero */
    d = tick(MAX35 - 5000.0f, 8500, 10000);
    CHECK(d.write);
    CHECK_I(d.target_w, 9000);
}

static void test_exactly_at_the_limit_does_nothing(void)
{
    reset_all();
    sls_decision_t d = tick(MAX35, 8500, 10000);
    CHECK(!d.write);                  /* not over, and no headroom either */
}

static void test_import_is_not_an_export_excess(void)
{
    reset_all();
    /* Negative export = the house is importing. Nothing to protect against,
     * and it counts as headroom like any other value under the limit. */
    sls_decision_t d = tick(-4000.0f, 8500, 10000);
    CHECK(!d.write);
    t += (uint32_t)SLS_RESTORE_HOLD_S * 1000u;
    d = tick(-4000.0f, 8500, 10000);
    CHECK(d.write);
    CHECK_I(d.target_w, 9000);
}

int main(void)
{
    RUN(test_within_limit_writes_nothing);
    RUN(test_over_the_limit_throttles);
    RUN(test_correction_is_measured_from_the_applied_power);
    RUN(test_throttle_converges_instead_of_oscillating);
    RUN(test_a_large_excess_ignores_the_rate_limit);
    RUN(test_a_small_excess_waits_for_the_rate_limit);
    RUN(test_throttle_never_goes_below_the_inverter_minimum);

    RUN(test_restore_needs_headroom_not_just_being_under);
    RUN(test_restore_needs_the_headroom_to_last);
    RUN(test_restore_steps_up_once_the_hold_has_passed);
    RUN(test_restore_walks_up_one_step_per_hold);
    RUN(test_restore_stops_at_the_user_setpoint);
    RUN(test_restore_lands_exactly_on_an_odd_user_setpoint);
    RUN(test_an_excess_cancels_a_running_hold);

    RUN(test_lowering_the_setpoint_below_the_throttle_is_respected);
    RUN(test_the_decision_leaves_the_user_setpoint_alone);

    RUN(test_the_clock_may_wrap);
    RUN(test_exactly_at_the_limit_does_nothing);
    RUN(test_import_is_not_an_export_excess);

    return t_report("sls_guard");
}

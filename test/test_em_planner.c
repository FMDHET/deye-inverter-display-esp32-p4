#include "harness.h"

/* The energy manager's planner. It decides when the house battery charges
 * from the grid and when it must not discharge -- decisions that cost or save
 * real money every night and that nobody can watch happen: the interesting
 * cases ("the sun would have filled it anyway", "the battery has the energy
 * but would waste it on the cheap afternoon") need a day of prices and
 * weather each. As a pure function they cost microseconds here. */
#include "em_planner.c"

#include <stdlib.h>
#include <time.h>

static em_plan_in_t in;
static em_plan_t    out;

/* A neutral house: flat 30 ct, no PV, 0.5 kW load, 10 kWh battery at 50 %. */
static void base_in(uint16_t n)
{
    memset(&in, 0, sizeof(in));
    in.t0 = 1792000800;                  /* any quarter hour */
    in.n  = n;
    for (int i = 0; i < EM_MAX_SLOTS; i++) {
        in.price_ct[i] = i < n ? 30.0f : NAN;
        in.pv_kw[i]    = i < n ? 0.0f  : NAN;
        in.load_kw[i]  = i < n ? 0.5f  : NAN;
    }
    in.soc_pct       = 50.0f;
    in.cap_kwh       = 10.0f;
    in.soc_min_pct   = 10.0f;
    in.soc_max_pct   = 90.0f;
    in.charge_kw     = 5.0f;
    in.dis_kw_max    = 5.0f;
    in.min_spread_ct = 2.0f;
    in.cycle_cost_ct = 2.0f;
    in.feedin_ct     = 8.0f;
    in.pv_confidence = 1.0f;
}

static void set(int from, int to, float price, float pv, float load)
{
    for (int i = from; i < to; i++) {
        in.price_ct[i] = price; in.pv_kw[i] = pv; in.load_kw[i] = load;
    }
}

static int count_act(uint8_t act, int from, int to)
{
    int c = 0;
    for (int i = from; i < to; i++) c += out.s[i].act == act;
    return c;
}

static float import_kwh(int from, int to)
{
    float s = 0.0f;
    for (int i = from; i < to; i++)
        if (out.s[i].grid_kw > 0.0f) s += out.s[i].grid_kw * 0.25f;
    return s;
}

/* Invariants every plan must keep, whatever the scenario. */
static void check_invariants(void)
{
    float smin = in.soc_min_pct;
    CHECK(out.cost_plan_ct <= out.cost_base_ct + 1e-3f);
    for (int i = 0; i < out.n; i++) {
        const em_slot_plan_t *p = &out.s[i];
        CHECK(p->act <= EM_ACT_CHARGE);
        CHECK(p->soc_end <= 100.0f + 1e-3f);
        /* Never below the floor -- unless it started there and only PV-less
         * holds kept it, in which case it can only have stayed. */
        float start = in.soc_pct < smin ? in.soc_pct : smin;
        CHECK(p->soc_end >= start - 1e-3f);
        if (p->act == EM_ACT_NORMAL) {
            CHECK_I(p->for_slot, -1);
        } else {
            /* Every move names the later, dearer slot it was made for --
             * or the energy left after the horizon. */
            CHECK(p->for_slot == EM_FOR_END || (p->for_slot > i && p->for_slot < out.n));
            if (p->for_slot > i && p->for_slot < out.n)
                CHECK(in.price_ct[p->for_slot] > in.price_ct[i]);
        }
        if (p->act == EM_ACT_CHARGE)
            CHECK(p->charge_kw > 0.0f && p->charge_kw <= in.charge_kw + 1e-4f);
    }
}

/* ----------------------------- basics --------------------------------- */

static void test_flat_price_plans_nothing(void)
{
    base_in(96);
    em_plan_compute(&in, &out);
    CHECK_I(out.n, 96);
    CHECK_I(count_act(EM_ACT_NORMAL, 0, 96), 96);
    CHECK_F(out.cost_plan_ct, out.cost_base_ct, 1e-3);
    CHECK(out.end_value_ct >= 0.0f);
    check_invariants();
}

static void test_horizon_ends_at_first_nan(void)
{
    base_in(96);
    in.pv_kw[40] = NAN;                     /* the forecast stops here */
    in.price_ct[60] = NAN;
    em_plan_compute(&in, &out);
    CHECK_I(out.n, 40);
    CHECK_I(out.s[40].act, EM_ACT_NORMAL);
    CHECK_I(out.s[40].for_slot, -1);
    CHECK_F(out.s[40].soc_end, 0.0f, 0);

    base_in(96);
    in.price_ct[0] = NAN;                   /* nothing known: nothing planned */
    em_plan_compute(&in, &out);
    CHECK_I(out.n, 0);
    CHECK_I(out.iterations, 0);
}

static void test_no_capacity_plans_nothing(void)
{
    base_in(48);
    set(0, 16, 10.0f, 0.0f, 0.5f);
    set(32, 48, 60.0f, 0.0f, 2.0f);
    in.cap_kwh = 0.0f;
    em_plan_compute(&in, &out);
    CHECK_I(count_act(EM_ACT_NORMAL, 0, 48), 48);
    CHECK_I(out.iterations, 0);
    CHECK_F(out.cost_plan_ct, out.cost_base_ct, 1e-4);
    CHECK_F(out.s[40].grid_kw, 2.0f, 1e-5);   /* the house straight from the grid */
    CHECK_F(out.end_value_ct, 0.0f, 0);

    in.cap_kwh = 10.0f;                       /* unknown SoC: same */
    in.soc_pct = NAN;
    em_plan_compute(&in, &out);
    CHECK_I(count_act(EM_ACT_NORMAL, 0, 48), 48);
}

/* ------------------------- the night charge ---------------------------- */

/* 12 h: night 4 h (18-20 ct, 0.3 kW), day 4 h (22 ct, 0.5 kW), evening 4 h
 * (45 ct, 2 kW). 20 kWh battery standing at its floor. */
static void night_day_evening(float evening_ct)
{
    base_in(48);
    in.cap_kwh = 20.0f;
    in.soc_pct = 10.0f;
    in.charge_kw = 10.0f;
    set(0, 16, 20.0f, 0.0f, 0.3f);
    set(4, 8, 18.0f, 0.0f, 0.3f);         /* the cheapest hour */
    set(16, 32, 22.0f, 0.0f, 0.5f);
    set(32, 48, evening_ct, 0.0f, 2.0f);
}

static void test_cheap_night_charges_for_the_evening(void)
{
    night_day_evening(45.0f);
    em_plan_compute(&in, &out);
    check_invariants();

    /* The evening is served from the battery ... */
    float base_evening = 8.0f;            /* 2 kW x 4 h without a plan */
    CHECK(import_kwh(32, 48) < 0.05f * base_evening);
    /* ... with the energy bought in the cheapest hour only ... */
    CHECK(count_act(EM_ACT_CHARGE, 0, 48) > 0);
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 4) + count_act(EM_ACT_CHARGE, 8, 48), 0);
    /* ... and not more than the evening needs: what is left at the end is
     * a rounding remainder of the 0.25 kWh quantum, not a second evening. */
    CHECK(out.s[47].soc_end < in.soc_min_pct + 3.0f);
    /* The day in between (22 ct) is not worth serving from 18 ct energy
     * after losses and wear, so the charge must be held through it. */
    CHECK(count_act(EM_ACT_HOLD, 16, 32) + count_act(EM_ACT_CHARGE, 16, 32) > 0);
    CHECK(out.cost_plan_ct < out.cost_base_ct - 100.0f);
    for (int i = 0; i < 48; i++)
        if (out.s[i].act == EM_ACT_CHARGE) CHECK(out.s[i].for_slot >= 32);
}

static void test_spread_below_minimum_plans_nothing(void)
{
    /* 18 -> 30 ct pays after losses and wear (about 7 ct/kWh) ... */
    night_day_evening(30.0f);
    em_plan_compute(&in, &out);
    CHECK(count_act(EM_ACT_CHARGE, 0, 48) > 0);

    /* ... but not when the operator asks for 15 ct of spread. */
    night_day_evening(30.0f);
    in.min_spread_ct = 15.0f;
    em_plan_compute(&in, &out);
    CHECK_I(count_act(EM_ACT_NORMAL, 0, 48), 48);
    CHECK_F(out.cost_plan_ct, out.cost_base_ct, 1e-4);
    check_invariants();

    /* And a spread that the losses eat is never worth it, whatever the
     * operator's minimum: 18 ct bought is 23 ct delivered. */
    night_day_evening(22.0f);
    in.min_spread_ct = 0.0f;
    in.cycle_cost_ct = 0.0f;
    em_plan_compute(&in, &out);
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 48), 0);
}

static void test_morning_sun_makes_night_charge_pointless(void)
{
    /* Same night and evening, but a sunny morning fills the battery before
     * the peak. Charging at night would only push PV into the grid at 8 ct. */
    night_day_evening(45.0f);
    set(16, 32, 30.0f, 7.0f, 0.5f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 48), 0);
    CHECK(import_kwh(32, 48) < 0.1f);

    /* Even when the planner only trusts 80 % of that sun. */
    in.pv_confidence = 0.8f;
    em_plan_compute(&in, &out);
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 48), 0);
}

/* Grid charge planned for a slot inside the horizon (not for the end). */
static float charged_for_horizon_kwh(void)
{
    float s = 0.0f;
    for (int i = 0; i < out.n; i++)
        if (out.s[i].act == EM_ACT_CHARGE && out.s[i].for_slot >= 0)
            s += out.s[i].charge_kw * 0.25f;
    return s;
}

static void test_pv_confidence_buys_insurance(void)
{
    /* Sun that covers the evening with room to spare when trusted fully: no
     * grid charge for the evening. Trusted at half it does not, and the
     * night has to make up the rest. (Charges for the END may happen in
     * both: this 12 h horizon has a median of 30 ct, so cheap night energy
     * left over is worth keeping -- that is not what this test is about.) */
    night_day_evening(45.0f);
    set(16, 32, 30.0f, 3.5f, 0.5f);
    em_plan_compute(&in, &out);
    CHECK_F(charged_for_horizon_kwh(), 0.0f, 0);
    CHECK(import_kwh(32, 48) < 0.1f);

    in.pv_confidence = 0.5f;
    em_plan_compute(&in, &out);
    CHECK(charged_for_horizon_kwh() > 1.0f);
    CHECK(import_kwh(32, 48) < 0.1f);
    check_invariants();

    /* An unset confidence (0) is full trust, not "no sun". */
    in.pv_confidence = 0.0f;
    em_plan_compute(&in, &out);
    CHECK_F(charged_for_horizon_kwh(), 0.0f, 0);
}

static void test_end_value_keeps_cheap_energy(void)
{
    /* Nothing inside the horizon imports -- the battery covers it all -- but
     * the night load is small (0.2 kW, the inverter's own 90 W make that 30 %
     * loss) and cheap, while energy kept for tomorrow is worth the median.
     * The plan keeps it: HOLD at the cheap slots, for the end. */
    base_in(32);
    in.soc_pct = 90.0f;
    in.min_spread_ct = 1.0f;
    set(0, 16, 15.0f, 0.0f, 0.2f);
    set(16, 32, 35.0f, 0.0f, 0.5f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK(count_act(EM_ACT_HOLD, 0, 16) > 0);
    for (int i = 0; i < 16; i++)
        if (out.s[i].act != EM_ACT_NORMAL) CHECK_I(out.s[i].for_slot, EM_FOR_END);
    CHECK_I(count_act(EM_ACT_HOLD, 16, 32), 0);   /* the dear slots use it */
    CHECK(out.end_value_ct > 0.0f);
}

/* ------------------------------ HOLD ----------------------------------- */

static void test_hold_beats_charge_when_the_energy_is_there(void)
{
    /* 5 kWh usable. Night (28 ct, 0.3 kW), afternoon (30 ct, 1 kW) and the
     * evening (50 ct, 2 kW) would together take 6.8 kWh DC: the battery runs
     * dry in the evening. It has enough for the evening alone -- so the
     * right move is to keep it, not to buy more at 28 ct and lose 15 % of it
     * in the conversion. */
    base_in(20);
    in.soc_pct = 60.0f;
    set(0, 4, 28.0f, 0.0f, 0.3f);
    set(4, 12, 30.0f, 0.0f, 1.0f);
    set(12, 20, 50.0f, 0.0f, 2.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 20), 0);
    CHECK(count_act(EM_ACT_HOLD, 0, 12) > 0);
    CHECK(import_kwh(12, 20) < 0.05f);
    for (int i = 0; i < 12; i++)
        if (out.s[i].act == EM_ACT_HOLD) CHECK(out.s[i].for_slot >= 12);
    CHECK(out.cost_plan_ct < out.cost_base_ct);
}

static void test_hold_bridges_cheap_afternoon(void)
{
    /* Full battery at noon, flat 30 ct afternoon, 40 ct evening: holding
     * one afternoon slot moves the empty point -- and so the saved energy --
     * into the evening. */
    base_in(32);
    in.soc_pct = 60.0f;                   /* 5 kWh usable */
    set(0, 16, 30.0f, 0.0f, 1.0f);
    set(16, 32, 40.0f, 0.0f, 1.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK(count_act(EM_ACT_HOLD, 0, 16) > 0);
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 32), 0);
    /* The evening needs 4.4 kWh DC of the 5: all of it from the battery. */
    CHECK(import_kwh(16, 32) < 0.05f);
}

/* ------------------------------ limits --------------------------------- */

static float top_soc(int from, int to)
{
    float top = 0.0f;
    for (int i = from; i < to; i++) if (out.s[i].soc_end > top) top = out.s[i].soc_end;
    return top;
}

static void test_grid_charge_stops_at_soc_max_pv_fills_to_full(void)
{
    /* Night 18 ct, then an evening at 60 ct that needs more than the whole
     * battery. Without sun the grid fills it -- to soc_max and no further. */
    base_in(32);
    in.soc_pct = 10.0f;
    in.charge_kw = 10.0f;
    set(0, 8, 18.0f, 0.0f, 0.3f);
    set(8, 16, 30.0f, 0.0f, 0.5f);
    set(16, 32, 60.0f, 0.0f, 5.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK(count_act(EM_ACT_CHARGE, 0, 8) > 0);
    CHECK_F(top_soc(0, 32), in.soc_max_pct, 0.01);

    /* With a sunny day in between, the sun may take it to 100 % -- and the
     * night charge leaves exactly the room the sun needs: the grid charge
     * stops below soc_max and almost no PV goes to the grid. */
    set(8, 16, 30.0f, 3.0f, 0.5f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK(count_act(EM_ACT_CHARGE, 0, 8) > 0);
    CHECK(top_soc(0, 8) <= in.soc_max_pct + 1e-3f);
    CHECK_F(top_soc(0, 32), 100.0f, 0.01);
    float exported = 0.0f;
    for (int i = 8; i < 16; i++) if (out.s[i].grid_kw < 0.0f) exported -= out.s[i].grid_kw * 0.25f;
    CHECK(exported < 0.25f);                     /* less than one charge quantum */

    /* Above soc_max already: no grid charge, the sun still fills it. */
    in.soc_pct = 95.0f;
    em_plan_compute(&in, &out);
    CHECK_I(count_act(EM_ACT_CHARGE, 0, 32), 0);
    CHECK_F(top_soc(0, 32), 100.0f, 0.01);
}

static void test_soc_min_is_never_undercut(void)
{
    base_in(32);
    in.soc_pct = 30.0f;
    in.soc_min_pct = 20.0f;
    set(0, 32, 40.0f, 0.0f, 3.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    float lo = 100.0f;
    for (int i = 0; i < 32; i++) if (out.s[i].soc_end < lo) lo = out.s[i].soc_end;
    CHECK(lo >= 20.0f - 1e-3f);
    CHECK(lo <  20.5f);                          /* and it does go down to it */

    /* Starting below the floor: no discharge at all. */
    in.soc_pct = 15.0f;
    em_plan_compute(&in, &out);
    for (int i = 0; i < 32; i++) CHECK_F(out.s[i].soc_end, 15.0f, 1e-3);
}

static void test_tiny_load_is_not_worth_the_idle_loss(void)
{
    /* Loss model sanity: 0.05 kW from the battery costs 0.14 kW DC. */
    base_in(4);
    in.soc_pct = 50.0f;
    set(0, 4, 30.0f, 0.0f, 0.05f);
    em_plan_compute(&in, &out);
    float dc = (50.0f - out.s[3].soc_end) / 100.0f * 10.0f;
    CHECK_F(dc, 4.0f * 0.25f * (0.05f + 0.09f + 0.004f * 0.0025f), 1e-4);
    CHECK_F(out.s[0].grid_kw, 0.0f, 1e-5);
}

/* --------------------------- randomised -------------------------------- */

static uint32_t rng = 12345;
static float frand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 16777216.0f;
}

/* Two days that look like Tibber: a night trough, a morning and an evening
 * peak, noise; PV as a bell around noon; load with an evening bump. */
static void realistic_two_days(float pv_peak, float noise)
{
    base_in(EM_MAX_SLOTS);
    in.cap_kwh = 15.0f;
    in.soc_pct = 20.0f + 60.0f * frand();
    in.charge_kw = 6.0f;
    in.pv_confidence = 0.8f;
    for (int i = 0; i < EM_MAX_SLOTS; i++) {
        float h = (float)(i % 96) / 4.0f;
        float p = 25.0f + 6.0f * sinf((h - 9.0f) / 24.0f * 6.2832f)
                + 8.0f * expf(-(h - 19.0f) * (h - 19.0f) / 4.0f)
                + 5.0f * expf(-(h - 7.5f) * (h - 7.5f) / 2.0f)
                - 6.0f * expf(-(h - 13.0f) * (h - 13.0f) / 6.0f);
        in.price_ct[i] = p + noise * (frand() - 0.5f);
        float s = (h - 13.0f) / 3.5f;
        in.pv_kw[i] = h > 6.0f && h < 20.0f ? pv_peak * expf(-s * s) : 0.0f;
        in.load_kw[i] = 0.35f + 0.4f * frand()
                      + 1.5f * expf(-(h - 19.0f) * (h - 19.0f) / 3.0f);
    }
}

static void test_random_scenarios_keep_invariants(void)
{
    for (int r = 0; r < 60; r++) {
        realistic_two_days(8.0f * frand(), 20.0f * frand());
        in.soc_min_pct = 5.0f + 20.0f * frand();
        in.soc_max_pct = in.soc_min_pct + (100.0f - in.soc_min_pct) * frand();
        in.feedin_ct = 10.0f * frand();
        in.min_spread_ct = 5.0f * frand();
        if (r % 7 == 0) in.price_ct[(int)(frand() * 190)] = -5.0f; /* negative price */
        if (r % 2) {
            /* prices for part of it, the forecast for the rest */
            uint16_t np = (uint16_t)(16 + frand() * 120);
            for (int i = np; i < EM_MAX_SLOTS; i++) in.price_ct[i] = NAN;
            in.n = np;
            in.n_ext = (uint16_t)(EM_MAX_SLOTS - np);
        }
        em_plan_compute(&in, &out);
        if (r % 2) CHECK_I(out.n, in.n);
        check_invariants();
        /* Grid charging never lifts the battery above soc_max on its own:
         * a CHARGE slot without PV surplus ends at or below the ceiling,
         * or where it started. */
        for (int i = 0; i < out.n; i++) {
            if (out.s[i].act != EM_ACT_CHARGE) continue;
            float pv = in.pv_kw[i] * 0.8f;
            float before = i ? out.s[i - 1].soc_end : in.soc_pct;
            if (pv <= in.load_kw[i])
                CHECK(out.s[i].soc_end <= fmaxf(in.soc_max_pct, before) + 1e-3f);
        }
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void test_runtime_full_horizon(void)
{
    /* The device runs at 400 MHz with a single-precision FPU; the host is
     * maybe 20-50x faster. 50 ms here is a comfortable second on the P4,
     * and the plan is recomputed every quarter hour at most. */
    double worst = 0.0;
    uint16_t it_max = 0;
    for (int r = 0; r < 20; r++) {
        realistic_two_days(3.0f + 6.0f * frand(), 25.0f);
        in.soc_pct = 10.0f;
        in.min_spread_ct = 1.0f;
        double t0 = now_ms();
        em_plan_compute(&in, &out);
        double dt = now_ms() - t0;
        if (dt > worst) worst = dt;
        if (out.iterations > it_max) it_max = out.iterations;
        CHECK_I(out.n, EM_MAX_SLOTS);
        CHECK(out.iterations <= EM_PLAN_MAX_ITER);
        check_invariants();
    }
    /* Adversarial: pure noise prices, no spread, every slot a candidate. */
    for (int r = 0; r < 5; r++) {
        base_in(EM_MAX_SLOTS);
        in.cap_kwh = 20.0f; in.soc_pct = 10.0f; in.charge_kw = 10.0f;
        in.min_spread_ct = 0.0f; in.cycle_cost_ct = 0.0f;
        for (int i = 0; i < EM_MAX_SLOTS; i++) {
            in.price_ct[i] = 5.0f + 60.0f * frand();
            in.load_kw[i]  = 0.2f + 3.0f * frand();
        }
        double t0 = now_ms();
        em_plan_compute(&in, &out);
        double dt = now_ms() - t0;
        if (dt > worst) worst = dt;
        if (out.iterations > it_max) it_max = out.iterations;
        check_invariants();
    }
    printf("    em_planner: 192 slots, worst %.2f ms, max %u passes\n",
           worst, (unsigned)it_max);
    CHECK(worst < 50.0);
}

/* --------------------------- after the horizon --------------------------- */

static float charged_for_end_kwh(void)
{
    float s = 0.0f;
    for (int i = 0; i < out.n; i++)
        if (out.s[i].act == EM_ACT_CHARGE && out.s[i].for_slot == EM_FOR_END)
            s += out.s[i].charge_kw * 0.25f;
    return s;
}

/* Prices known until midnight only (before Tibber publishes tomorrow): an
 * evening at 40 ct, then a cheap late evening at 10 ct. What the battery
 * should take into the night depends on tomorrow -- the operator's point:
 * "if today's sun makes 60 kWh, that has to last two days", and the other
 * way round: no buying for a day the sun covers anyway. */
static void evening_then_tomorrow(float tomorrow_pv_kw)
{
    base_in(16);
    in.soc_pct = 15.0f;                       /* 1.5 kWh, floor 1 kWh */
    set(0, 8, 40.0f, 0.0f, 0.5f);
    set(8, 16, 10.0f, 0.0f, 0.5f);
    in.n_ext = 96;                            /* tomorrow, no prices yet */
    for (int i = 16; i < 16 + 96; i++) {
        int h = (i - 16) / 4;                 /* hour of tomorrow */
        in.price_ct[i] = NAN;
        in.load_kw[i]  = 0.8f;
        in.pv_kw[i]    = (h >= 6 && h < 18) ? tomorrow_pv_kw : 0.0f;
    }
}

static void test_tomorrows_sun_limits_the_night_charge(void)
{
    /* Cloudy tomorrow: what is bought cheaply tonight saves the median price
     * all day tomorrow -- charge up to soc_max. */
    evening_then_tomorrow(0.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    float cloudy = charged_for_end_kwh();
    CHECK(cloudy > 7.5f);
    CHECK(out.s[15].soc_end > 85.0f);

    /* Sunny tomorrow (4 kW from 6 to 18): the sun refills the battery in the
     * morning, so carried-over energy is worth only the night until the sun
     * comes -- 6 h x 0.8 kW plus losses. Charge for that, not to soc_max. */
    evening_then_tomorrow(4.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    float sunny = charged_for_end_kwh();
    CHECK(sunny > 3.0f);
    CHECK(sunny < 7.0f);
    CHECK(sunny < cloudy - 2.0f);
    CHECK(out.s[15].soc_end > 50.0f && out.s[15].soc_end < 75.0f);

    /* Without the forecast after the horizon (n_ext 0) the end is valued
     * flat, as before: like the cloudy day. */
    evening_then_tomorrow(4.0f);
    in.n_ext = 0;
    em_plan_compute(&in, &out);
    CHECK(charged_for_end_kwh() > 7.5f);

    /* A forecast that stops early counts up to its first gap only; one
     * shorter than an hour is ignored (flat value). */
    evening_then_tomorrow(4.0f);
    in.pv_kw[16 + 2] = NAN;
    em_plan_compute(&in, &out);
    CHECK(charged_for_end_kwh() > 7.5f);
    check_invariants();

    /* Enough in the battery for the night already, sun tomorrow: next to
     * nothing is bought, and the dear evening uses the battery -- no HOLD
     * for the end in the 40 ct slots. */
    evening_then_tomorrow(4.0f);
    in.soc_pct = 70.0f;
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK_I(count_act(EM_ACT_HOLD, 0, 8), 0);
    CHECK(charged_for_end_kwh() < 1.5f);
}

/* ------------------------------ saturation --------------------------- */

static void test_charge_slows_near_full(void)
{
    /* The simulator itself: below EM_TAPER_FROM_PCT the full charge power
     * goes in, above it less, at 100 % only EM_TAPER_END of it. */
    base_in(4);
    in.cap_kwh = 100.0f; in.soc_max_pct = 100.0f;
    ctx_t c;
    ctx_init(&c, &in, 4);
    step_t st;
    CHECK_F(accept_kw(&c, 100.0f), 5.0f * EM_TAPER_END, 1e-4);
    CHECK_F(accept_kw(&c, 95.0f), 3.0f, 1e-4);
    CHECK(isinf(accept_kw(&c, 89.0f)));
    float e50 = slot_step(&c, &in, 0, 50.0f, EM_ACT_CHARGE, 5.0f, &st);
    CHECK(e50 - 50.0f > 1.0f);                    /* ~5 kW minus losses   */
    float e95 = slot_step(&c, &in, 0, 95.0f, EM_ACT_CHARGE, 5.0f, &st);
    /* Starts at 3 kW and slows on: a bit less than 0.75 kWh. */
    CHECK(e95 - 95.0f < 0.75f && e95 - 95.0f > 0.7f);
    CHECK(st.g_used < 5.0f * 0.7f);               /* takes less from the grid */
    /* PV is capped alike. */
    in.pv_kw[0] = 8.0f;
    float pv95 = slot_step(&c, &in, 0, 95.0f, EM_ACT_NORMAL, 0.0f, &st);
    CHECK(fabsf((pv95 - 95.0f) - (e95 - 95.0f)) < 1e-3f);

    /* The plan: half an hour of cheap power and a dear evening. Without the
     * taper half an hour at 10 kW would take 20 kWh from 80 % to 100 %; with
     * it the plan knows the last percent comes slowly -- it does not count
     * them. */
    base_in(24);
    in.cap_kwh = 20.0f;
    in.soc_pct = 80.0f; in.soc_max_pct = 100.0f; in.charge_kw = 10.0f;
    set(0, 2, 15.0f, 0.0f, 0.3f);
    set(2, 8, 30.0f, 0.0f, 0.3f);
    set(8, 24, 45.0f, 0.0f, 3.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK(count_act(EM_ACT_CHARGE, 0, 2) == 2);
    CHECK(out.s[1].soc_end < 99.0f);
    CHECK(out.s[1].soc_end > 95.0f);
}

/* A long cheap stretch, then a dearer hour, then the expensive evening:
 * the battery is filled in the cheap stretch -- started early enough that
 * the slow end still fits -- not topped up in the dearer hour. */
static void test_fills_early_rather_than_buying_dearer(void)
{
    base_in(48);
    in.cap_kwh = 20.0f; in.soc_pct = 20.0f; in.soc_max_pct = 100.0f;
    in.charge_kw = 10.0f;
    set(0, 16, 18.7f, 0.0f, 0.4f);
    set(16, 20, 21.0f, 0.0f, 0.4f);
    set(20, 48, 27.0f, 0.0f, 3.0f);
    em_plan_compute(&in, &out);
    check_invariants();
    CHECK_I(count_act(EM_ACT_CHARGE, 16, 20), 0);
    CHECK(out.s[15].soc_end > 97.0f);
}

int main(void)
{
    RUN(test_flat_price_plans_nothing);
    RUN(test_horizon_ends_at_first_nan);
    RUN(test_no_capacity_plans_nothing);
    RUN(test_cheap_night_charges_for_the_evening);
    RUN(test_spread_below_minimum_plans_nothing);
    RUN(test_morning_sun_makes_night_charge_pointless);
    RUN(test_pv_confidence_buys_insurance);
    RUN(test_end_value_keeps_cheap_energy);
    RUN(test_hold_beats_charge_when_the_energy_is_there);
    RUN(test_hold_bridges_cheap_afternoon);
    RUN(test_grid_charge_stops_at_soc_max_pv_fills_to_full);
    RUN(test_soc_min_is_never_undercut);
    RUN(test_tiny_load_is_not_worth_the_idle_loss);
    RUN(test_tomorrows_sun_limits_the_night_charge);
    RUN(test_charge_slows_near_full);
    RUN(test_fills_early_rather_than_buying_dearer);
    RUN(test_random_scenarios_keep_invariants);
    RUN(test_runtime_full_horizon);
    return t_report("em_planner");
}

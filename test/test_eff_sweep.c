#include "harness.h"
#include "fakes.h"
#include "esp_timer.h"

/* The efficiency sweep drives the inverter for an hour or two without anyone
 * watching. What matters is what it does when things go differently than
 * planned: the user takes over, the battery runs into a limit, the Deye goes
 * silent, a write does not take. Nobody wants to wait for those on the
 * device, so a simulated inverter answers here. */
#include "eff_sweep.c"

/* ----------------------- a simulated inverter -------------------------
 * deye_ctrl as the sweep sees it, plus a battery that follows the forced
 * mode at a fixed efficiency, and a house drawing a constant load. */
static deye_mode_t f_mode;
static int         f_user;
static int         f_applies, f_retunes, f_normals;
static uint32_t    f_writes;
static uint8_t     f_failed;
static mb_deye_live_t f_live;
static float       f_soc;
static bool        f_silent;   /* the Deye stops answering */
#define HOUSE_W    500.0f
#define ETA_DIS    0.94f
#define ETA_CHG    0.95f
#define AC_GAIN    1.02f      /* the grid meter sees 2 % more than the Deye says */

deye_mode_t deye_ctrl_get_mode(void)       { return f_mode; }
int         deye_ctrl_get_user_power(void) { return f_user; }
void deye_ctrl_get_status(deye_ctrl_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->mode = f_mode; o->user_power_w = f_user; o->writes = f_writes; o->failed = f_failed;
}
esp_err_t deye_ctrl_apply(deye_mode_t m, int w)
{
    f_mode = m; f_user = w; f_applies++;
    if (m == DEYE_MODE_NORMAL) f_normals++;
    f_writes += m == DEYE_MODE_FORCE_CHARGE ? 15 : (m == DEYE_MODE_FORCE_DISCHARGE ? 2 : 17);
    return ESP_OK;
}
esp_err_t deye_ctrl_apply_sweep(deye_mode_t m, int w)
{
    CHECK(m != DEYE_MODE_NORMAL);       /* Normal goes through the normal range */
    return deye_ctrl_apply(m, w);
}
esp_err_t deye_ctrl_retune(int w)
{
    if (f_mode == DEYE_MODE_NORMAL) return ESP_ERR_INVALID_STATE;
    f_user = w; f_retunes++;
    f_writes += f_mode == DEYE_MODE_FORCE_CHARGE ? 2 : 1;
    return ESP_OK;
}
void modbus_rtu_get_deye_live(mb_deye_live_t *o) { *o = f_live; }

static void sim_reset(float soc)
{
    memset(&s_st, 0, sizeof(s_st));
    s_stop_req = false;
    s_task     = NULL;
    s_lock     = NULL;
    f_mode = DEYE_MODE_NORMAL; f_user = 5000;
    f_applies = f_retunes = f_normals = 0; f_writes = 0; f_failed = 0;
    f_soc = soc;
    f_silent = false;
    memset(&f_live, 0, sizeof(f_live));
    f_live.valid = f_live.online = true;
    f_live.bat_soc = soc;
    fake_time_set_ms(1000000);
    fake_grid_fresh = true;
}

/* One second. Every second second is a poll round: the inverter is at the
 * power deye_ctrl holds, and the round counts (eff_ok). */
static void sim_second(void)
{
    fake_time_advance_ms(1000);
    if (!f_silent && (fake_now_us / 1000000) % 2 == 0) {
        float ac = 0.0f, bat = 0.0f;   /* ac: + = delivers */
        if (f_mode == DEYE_MODE_FORCE_DISCHARGE) { ac = (float)f_user;  bat = ac / ETA_DIS; }
        if (f_mode == DEYE_MODE_FORCE_CHARGE)    { ac = -(float)f_user; bat = ac * ETA_CHG; }
        f_soc -= bat * 2.0f / 3600.0f / 300.0f;    /* 30 kWh battery: 300 Wh per % */
        f_live.bat_p     = bat;
        f_live.inv_total = -ac;
        f_live.pv_total  = 0.0f;
        f_live.bat_soc   = f_soc;
        f_live.eff_ok    = f_mode != DEYE_MODE_NORMAL;
        f_live.rounds++;
        f_live.age_ms    = 0;
        f_live.online    = true;
        fake_grid_w      = HOUSE_W - ac * AC_GAIN;
    } else if (f_silent) {
        f_live.online = false;
        f_live.age_ms += 1000;
    }
    float g = 0.0f;
    bool gok = modbus_tcp_grid_w_fresh(&g, MB_GRID_MAX_AGE_MS);
    sweep_tick((uint32_t)(fake_now_us / 1000), &f_live, gok, g);
}

static void sim_run(int seconds) { for (int i = 0; i < seconds; i++) sim_second(); }

static eff_sweep_req_t req3(void)
{
    eff_sweep_req_t rq = { .dirs = SWEEP_DIR_DIS | SWEEP_DIR_CHG, .npow = 3,
                           .pow = { 3000, 1000, 2000 },   /* unsorted on purpose */
                           .dwell_s = 120, .settle_s = 30, .soc_min = 20, .soc_max = 95 };
    return rq;
}

/* ------------------------------ refusals ------------------------------- */

static void test_refuses_over_a_forced_mode(void)
{
    sim_reset(60);
    f_mode = DEYE_MODE_FORCE_CHARGE;
    eff_sweep_req_t rq = req3();
    char err[100] = "";
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
    CHECK(strstr(err, "Zwangs") != NULL);
    CHECK_I(s_st.state, SWEEP_IDLE);
    CHECK_I(f_applies, 0);
}

static void test_refuses_bad_requests(void)
{
    char err[100];
    eff_sweep_req_t rq;

    sim_reset(60);
    rq = req3(); rq.pow[1] = 200;                   /* under DEYE_POWER_FLOOR */
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);

    rq = req3(); rq.dirs = 0;
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);

    rq = req3(); rq.settle_s = 110;                 /* leaves < 20 s to measure */
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);

    /* 12 steps x 10 min is past the 2-hour limit of one forced mode, which a
     * retune does not restart. */
    rq = req3(); rq.npow = 12; rq.dwell_s = 600;
    for (int i = 0; i < 12; i++) rq.pow[i] = (uint16_t)(1000 + 1000 * i);
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
    CHECK(strstr(err, "110 min") != NULL);

    f_live.online = false;
    rq = req3();
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
    CHECK_I(f_applies, 0);
}

static void test_refuses_when_the_soc_leaves_no_room(void)
{
    sim_reset(18);                                  /* under soc_min */
    eff_sweep_req_t rq = req3(); rq.dirs = SWEEP_DIR_DIS;
    char err[100];
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
}

/* ------------------------------ a whole run ----------------------------- */

static void test_a_full_sweep(void)
{
    sim_reset(60);                                  /* >= 50 %: discharge first */
    eff_sweep_req_t rq = req3();
    char err[100] = "";
    CHECK_I(eff_sweep_start(&rq, err, sizeof(err)), ESP_OK);
    CHECK_I(s_st.nsteps, 6);
    CHECK_I(s_st.step[0].mode, DEYE_MODE_FORCE_DISCHARGE);
    CHECK_I(s_st.step[0].target_w, 1000);           /* sorted */
    CHECK_I(s_st.step[2].target_w, 3000);
    CHECK_I(s_st.step[3].mode, DEYE_MODE_FORCE_CHARGE);

    sim_run(3 * 120 + 5);
    CHECK_I(s_st.cur, 3);
    /* One apply for the direction, then only retunes. */
    CHECK_I(f_retunes, 2);
    /* The direction change goes through Normal. */
    CHECK(s_st.paused);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);

    sim_run(SWEEP_PAUSE_S + 3 * 120 + 10);
    CHECK_I(s_st.state, SWEEP_DONE);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);              /* and it ends in Normal */
    CHECK_I(f_user, 5000);                          /* with the slider as it was */
    CHECK_I(f_retunes, 4);
    CHECK_I(f_applies, 4);                          /* dis, Normal, chg, Normal */

    for (int i = 0; i < 6; i++) {
        const eff_sweep_step_t *p = &s_st.step[i];
        CHECK_I(p->state, STEP_DONE);
        /* 90 s measured at a 2 s cadence; the settling rounds do not count. */
        CHECK(p->n >= 44 && p->n <= 46);
        float eta = p->out_sum / p->in_sum;
        CHECK_F(eta, p->mode == DEYE_MODE_FORCE_DISCHARGE ? ETA_DIS : ETA_CHG, 1e-4);
        CHECK_I(p->n_grid, p->n);
        /* The grid meter moves by the gain times the Deye's AC power. */
        float ac = p->ac_g_sum / p->n_grid, grid = p->grid_sum / p->n_grid;
        CHECK_F(grid, HOUSE_W - AC_GAIN * ac, 0.5);
    }
    CHECK_F(s_st.step[0].ac_sum / s_st.step[0].n, 1000, 0.01);
    CHECK_F(s_st.step[5].ac_sum / s_st.step[5].n, -3000, 0.01);
    CHECK_I(s_st.writes, 2 + 2 * 1 + 17 + 15 + 2 * 2 + 17);
}

/* The default the page sends: 500 W to 5 kW in 250 W steps -- nineteen of
 * them, the first two under the slider's 1000 W. */
static void test_the_default_steps(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    rq.npow = 0;
    for (int w = 500; w <= 5000; w += 250) rq.pow[rq.npow++] = (uint16_t)w;
    CHECK_I(rq.npow, 19);
    rq.dwell_s = 30; rq.settle_s = 10;
    char err[100] = "";
    CHECK_I(eff_sweep_start(&rq, err, sizeof(err)), ESP_OK);
    CHECK_I(s_st.nsteps, 38);
    sim_run(15);
    CHECK_I(f_mode, DEYE_MODE_FORCE_DISCHARGE);
    CHECK_I(f_user, 500);                           /* not clamped up to 1000 */
    sim_run(19 * 30 + SWEEP_PAUSE_S + 19 * 30 + 10);
    CHECK_I(s_st.state, SWEEP_DONE);
    CHECK_I(f_user, 5000);                          /* Normal with the slider value */
    CHECK_I(s_st.step[37].state, STEP_DONE);
    CHECK_F(s_st.step[1].ac_sum / s_st.step[1].n, 750, 0.01);

    rq.npow = 21;                                   /* one more than fits */
    sim_reset(60);
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
}

/* 30 s per step, 10 s of it settling. */
static void test_thirty_second_steps(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    rq.dwell_s = 30; rq.settle_s = 10;
    char err[100] = "";
    CHECK_I(eff_sweep_start(&rq, err, sizeof(err)), ESP_OK);
    sim_run(3 * 30 + SWEEP_PAUSE_S + 3 * 30 + 10);
    CHECK_I(s_st.state, SWEEP_DONE);
    for (int i = 0; i < 6; i++) {
        CHECK_I(s_st.step[i].state, STEP_DONE);
        CHECK(s_st.step[i].n >= 9 && s_st.step[i].n <= 11);   /* ten rounds in 20 s */
    }

    sim_reset(60);
    rq.dwell_s = 20;                                /* under 30 s: refused */
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
    rq.dwell_s = 30; rq.settle_s = 5;               /* settling under 10 s: refused */
    CHECK(eff_sweep_start(&rq, err, sizeof(err)) != ESP_OK);
}

static void test_a_low_soc_starts_with_charging(void)
{
    sim_reset(40);
    eff_sweep_req_t rq = req3();
    char err[100];
    CHECK_I(eff_sweep_start(&rq, err, sizeof(err)), ESP_OK);
    CHECK_I(s_st.step[0].mode, DEYE_MODE_FORCE_CHARGE);
    CHECK_I(s_st.step[3].mode, DEYE_MODE_FORCE_DISCHARGE);
}

/* ------------------------------ the exits ------------------------------- */

static void test_an_outside_change_ends_it_without_touching_the_inverter(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    char err[100];
    eff_sweep_start(&rq, err, sizeof(err));
    sim_run(50);
    CHECK_I(f_mode, DEYE_MODE_FORCE_DISCHARGE);

    /* The user forces a charge from the display. */
    f_mode = DEYE_MODE_FORCE_CHARGE; f_user = 4000;
    int normals = f_normals;
    sim_run(2);
    CHECK_I(s_st.state, SWEEP_ABORTED);
    CHECK_I(f_mode, DEYE_MODE_FORCE_CHARGE);        /* the user's choice stands */
    CHECK_I(f_normals, normals);
    CHECK_I(s_st.step[0].state, STEP_DONE);         /* what was measured is kept */
    CHECK_I(s_st.step[1].state, STEP_SKIPPED);

    /* A power change from MQTT is an outside change too. */
    sim_reset(60);
    eff_sweep_start(&rq, err, sizeof(err));
    sim_run(10);
    f_user = 7000;
    sim_run(1);
    CHECK_I(s_st.state, SWEEP_ABORTED);
}

static void test_the_soc_limit_ends_a_direction(void)
{
    sim_reset(23);
    eff_sweep_req_t rq = req3();
    rq.dirs = SWEEP_DIR_DIS;
    rq.pow[0] = 18000; rq.pow[1] = 19000; rq.pow[2] = 20000;
    char err[100];
    CHECK_I(eff_sweep_start(&rq, err, sizeof(err)), ESP_OK);

    /* 18 kW out of 300 Wh per percent: the first step takes the battery from
     * 23 to about 21 %, the second one reaches 20 % while still settling. */
    sim_run(400);
    CHECK_I(s_st.step[0].state, STEP_DONE);
    CHECK(s_st.step[0].n > 0);
    CHECK_I(s_st.step[1].state, STEP_SKIPPED);
    CHECK_I(s_st.step[2].state, STEP_SKIPPED);
    CHECK_I(s_st.state, SWEEP_DONE);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
    CHECK(f_soc > 19.5f);                           /* stopped at the limit, not past it */
}

static void test_stop_writes_normal(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    char err[100];
    eff_sweep_start(&rq, err, sizeof(err));
    sim_run(100);
    eff_sweep_stop();
    sim_run(1);
    CHECK_I(s_st.state, SWEEP_ABORTED);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
    CHECK(!s_stop_req);
    for (int i = 1; i < 6; i++) CHECK_I(s_st.step[i].state, STEP_SKIPPED);

    /* A stop with nothing running is forgotten, not kept for the next run. */
    eff_sweep_stop();
    sim_run(1);
    CHECK(!s_stop_req);
}

static void test_a_silent_deye_ends_it(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    char err[100];
    eff_sweep_start(&rq, err, sizeof(err));
    sim_run(40);
    f_silent = true;
    sim_run(SWEEP_OFFLINE_S - 2);
    CHECK_I(s_st.state, SWEEP_RUNNING);             /* a short gap is tolerated */
    sim_run(4);
    f_silent = false;
    CHECK_I(s_st.state, SWEEP_ABORTED);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
}

static void test_a_write_that_did_not_take_ends_it(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    char err[100];
    eff_sweep_start(&rq, err, sizeof(err));
    f_failed = 1;
    sim_run(rq.settle_s + 2);
    CHECK_I(s_st.state, SWEEP_ABORTED);
    CHECK_I(s_st.step[0].n, 0);                     /* nothing measured at the wrong power */
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
}

static void test_the_estimate_counts_down(void)
{
    sim_reset(60);
    eff_sweep_req_t rq = req3();
    char err[100];
    eff_sweep_start(&rq, err, sizeof(err));
    sim_run(1);
    eff_sweep_status_t st;
    eff_sweep_get(&st);
    CHECK(st.left_s >= 6 * 120 + SWEEP_PAUSE_S - 2 && st.left_s <= 6 * 120 + SWEEP_PAUSE_S);
    sim_run(130);
    eff_sweep_get(&st);
    CHECK(st.left_s < 5 * 120 + SWEEP_PAUSE_S);
}

int main(void)
{
    RUN(test_refuses_over_a_forced_mode);
    RUN(test_refuses_bad_requests);
    RUN(test_refuses_when_the_soc_leaves_no_room);
    RUN(test_a_full_sweep);
    RUN(test_thirty_second_steps);
    RUN(test_the_default_steps);
    RUN(test_a_low_soc_starts_with_charging);
    RUN(test_an_outside_change_ends_it_without_touching_the_inverter);
    RUN(test_the_soc_limit_ends_a_direction);
    RUN(test_stop_writes_normal);
    RUN(test_a_silent_deye_ends_it);
    RUN(test_a_write_that_did_not_take_ends_it);
    RUN(test_the_estimate_counts_down);
    return t_report("eff_sweep");
}

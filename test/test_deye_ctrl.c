#include "harness.h"
#include "fakes.h"
#include "fakes_rtu.h"

/* The unit under test. This is the file that writes into an inverter hanging
 * on the house installation, so "it looked right on the device once" is not
 * good enough -- especially for the parts that only happen after two hours or
 * after a restart, which nobody wants to sit through twice. */
#include "deye_ctrl.c"

static void reset_all(void)
{
    fake_rtu_reset();
    fake_deye_nvs_reset();
    fake_nvs_fail = false;
    fake_time_set_ms(10000);
    s_mode         = DEYE_MODE_NORMAL;
    s_power_w      = 5000;
    s_user_power_w = 5000;
    s_mode_ms      = (uint32_t)(fake_now_us / 1000);
    s_checked = s_failed = 0;
    s_undo_pending = false;
    s_undo_tries   = 0;
    s_task         = NULL;
}

/* ------------------------- what gets written --------------------------- */

static void test_normal_writes_the_reset_values(void)
{
    reset_all();
    deye_ctrl_write_regs(DEYE_MODE_NORMAL, 5000);

    CHECK_I(fake_rtu_reg(142), 2);          /* Zero Export to CT */
    CHECK_I(fake_rtu_reg(143), 20000);      /* full sell power   */
    CHECK_I(fake_rtu_reg(126), 5000);
    CHECK_I(fake_rtu_reg(127), 10);
    CHECK_I(fake_rtu_reg(128), 40);
    for (uint16_t r = 166; r <= 171; r++) CHECK_I(fake_rtu_reg(r), 13);
    for (uint16_t r = 172; r <= 177; r++) CHECK_I(fake_rtu_reg(r), 0);
    CHECK_I(s_failed, 0);
    CHECK_I(s_checked, 2);                  /* 142 and 143 are read back */
}

static void test_discharge_writes_mode_and_power(void)
{
    reset_all();
    deye_ctrl_write_regs(DEYE_MODE_FORCE_DISCHARGE, 3000);

    CHECK_I(fake_rtu_reg(142), 3);          /* Selling First */
    CHECK_I(fake_rtu_reg(143), 3000);
    CHECK_I(s_checked, 2);
    CHECK_I(s_failed, 0);
}

static void test_charge_converts_watt_to_ampere(void)
{
    reset_all();
    /* The inverter ignores the watt value in 126 and acts on the charge
     * current in 128 -- at roughly 50 V, so amps = watts / 50. Getting this
     * wrong charges at the wrong rate, which is exactly the kind of thing a
     * test should hold still. */
    deye_ctrl_write_regs(DEYE_MODE_FORCE_CHARGE, 5000);

    CHECK_I(fake_rtu_reg(126), 5000);
    CHECK_I(fake_rtu_reg(127), 99);
    CHECK_I(fake_rtu_reg(128), 100);        /* 5000 W / 50 V */
    for (uint16_t r = 166; r <= 171; r++) CHECK_I(fake_rtu_reg(r), 99);
    for (uint16_t r = 172; r <= 177; r++) CHECK_I(fake_rtu_reg(r), 1);
    CHECK_I(s_failed, 0);

    reset_all();
    deye_ctrl_write_regs(DEYE_MODE_FORCE_CHARGE, 1000);
    CHECK_I(fake_rtu_reg(128), 20);
}

/* --------------------- the read-back that was missing ------------------ */

static void test_a_silently_ignored_write_is_caught(void)
{
    /* The inverter answers "ok" and keeps its old value. Before the read-back
     * this looked exactly like success, and the display claimed a mode that
     * was never in force. */
    reset_all();
    fake_rtu_deaf = true;
    fake_rtu_set_reg(142, 2);               /* stays in Normal */
    fake_rtu_set_reg(143, 20000);

    deye_ctrl_write_regs(DEYE_MODE_FORCE_DISCHARGE, 3000);

    CHECK_I(s_checked, 2);
    CHECK_I(s_failed, 2);                   /* both decisive registers wrong */
}

static void test_a_failing_bus_is_caught(void)
{
    reset_all();
    fake_rtu_write_rc = -1;                 /* nothing gets through */
    deye_ctrl_write_regs(DEYE_MODE_FORCE_DISCHARGE, 3000);
    CHECK_I(s_failed, 2);

    reset_all();
    fake_rtu_read_rc = -1;                  /* write ok, read-back dead */
    deye_ctrl_write_regs(DEYE_MODE_FORCE_DISCHARGE, 3000);
    CHECK_I(s_failed, 2);                   /* unconfirmed counts as failed */
}

/* ------------------------------- apply --------------------------------- */

static void test_apply_clamps_and_remembers(void)
{
    reset_all();
    CHECK_I(deye_ctrl_apply(DEYE_MODE_FORCE_DISCHARGE, 999999), ESP_OK);
    CHECK_I(deye_ctrl_get_power(), DEYE_POWER_MAX);
    CHECK_I(deye_ctrl_get_user_power(), DEYE_POWER_MAX);

    deye_ctrl_apply(DEYE_MODE_FORCE_CHARGE, 1);
    CHECK_I(deye_ctrl_get_power(), DEYE_POWER_MIN);

    /* An unknown mode must not be applied as something else -- it becomes
     * Normal, the only safe answer. */
    deye_ctrl_apply((deye_mode_t)99, 3000);
    CHECK_I(deye_ctrl_get_mode(), DEYE_MODE_NORMAL);
}

static void test_apply_persists_so_a_restart_can_undo_it(void)
{
    reset_all();
    deye_ctrl_apply(DEYE_MODE_FORCE_CHARGE, 4000);
    CHECK_I(nvs_store_get_deye_mode(), DEYE_MODE_FORCE_CHARGE);
    CHECK_I(nvs_store_get_deye_power(), 4000);

    /* Back to Normal clears it again -- otherwise every later restart would
     * write Normal into an inverter that is already normal. */
    deye_ctrl_apply(DEYE_MODE_NORMAL, 4000);
    CHECK_I(nvs_store_get_deye_mode(), DEYE_MODE_NORMAL);
}

static void test_the_guard_never_touches_the_user_setpoint(void)
{
    /* The SLS export guard throttles the applied power. What the user asked
     * for has to survive that, or the slider in Home Assistant jumps around
     * on its own. */
    reset_all();
    deye_ctrl_apply(DEYE_MODE_FORCE_DISCHARGE, 12000);
    deye_ctrl_set_throttled(6000);

    CHECK_I(deye_ctrl_get_power(), 6000);
    CHECK_I(deye_ctrl_get_user_power(), 12000);

    deye_ctrl_set_throttled(999999);
    CHECK_I(deye_ctrl_get_power(), DEYE_POWER_MAX);
    CHECK_I(deye_ctrl_get_user_power(), 12000);
}

/* ------------------------- the two-hour limit -------------------------- */

static void test_status_counts_down_and_stops(void)
{
    reset_all();
    deye_ctrl_apply(DEYE_MODE_FORCE_DISCHARGE, 3000);

    deye_ctrl_status_t st;
    deye_ctrl_get_status(&st);
    CHECK_I(st.mode, DEYE_MODE_FORCE_DISCHARGE);
    CHECK_I(st.age_s, 0);
    CHECK_I(st.left_s, DEYE_FORCE_MAX_S);

    fake_time_advance_ms(30 * 60 * 1000);
    deye_ctrl_get_status(&st);
    CHECK_I(st.age_s, 30 * 60);
    CHECK_I(st.left_s, DEYE_FORCE_MAX_S - 30 * 60);

    /* Past the limit it reports zero left, never a negative or wrapped value
     * -- these are unsigned, an underflow here would read as 49 days. */
    fake_time_advance_ms(DEYE_FORCE_MAX_S * 1000);
    deye_ctrl_get_status(&st);
    CHECK_I(st.left_s, 0);

    /* In Normal there is nothing to count down. */
    deye_ctrl_apply(DEYE_MODE_NORMAL, 3000);
    deye_ctrl_get_status(&st);
    CHECK_I(st.left_s, 0);
}

/* --------------------- restart: undo, do not resume -------------------- */

static void test_start_undoes_a_stored_forced_mode(void)
{
    reset_all();
    fake_deye_nvs_set(DEYE_MODE_FORCE_CHARGE, 4000);

    deye_ctrl_start();

    /* It does NOT write yet: the RS485 master needs a moment after boot, so
     * the first timer tick does it. But it has to know that it must. */
    CHECK(s_undo_pending);
    CHECK_I(s_undo_mode, DEYE_MODE_FORCE_CHARGE);
    CHECK_I(fake_rtu_log_n, 0);
    /* The display shows what the INVERTER is in, not what we wish it were. */
    CHECK_I(deye_ctrl_get_mode(), DEYE_MODE_FORCE_CHARGE);
    CHECK_I(deye_ctrl_get_power(), 4000);
}

static void test_start_is_quiet_when_nothing_was_forced(void)
{
    reset_all();
    fake_deye_nvs_set(DEYE_MODE_NORMAL, 0);
    deye_ctrl_start();
    CHECK(!s_undo_pending);
    CHECK_I(fake_rtu_log_n, 0);
    CHECK_I(deye_ctrl_get_mode(), DEYE_MODE_NORMAL);
}

static void test_fallback_clears_the_store_only_once_confirmed(void)
{
    /* The important half. Clearing the stored mode before the inverter has
     * confirmed Normal would throw away the one thing that matters -- that it
     * is still forced -- the moment the write fails. */
    reset_all();
    fake_deye_nvs_set(DEYE_MODE_FORCE_CHARGE, 4000);
    s_mode = DEYE_MODE_FORCE_CHARGE;

    fake_rtu_deaf = true;                   /* inverter takes nothing */
    CHECK(!fall_back_to_normal("Test"));
    CHECK_I(nvs_store_get_deye_mode(), DEYE_MODE_FORCE_CHARGE);   /* kept! */

    /* Now it listens -- and only now is the entry allowed to go. */
    fake_rtu_deaf = false;
    CHECK(fall_back_to_normal("Test"));
    CHECK_I(nvs_store_get_deye_mode(), DEYE_MODE_NORMAL);
    CHECK_I(fake_rtu_reg(142), 2);
    CHECK_I(deye_ctrl_get_mode(), DEYE_MODE_NORMAL);
}

static void test_fallback_restarts_the_clock(void)
{
    reset_all();
    deye_ctrl_apply(DEYE_MODE_FORCE_DISCHARGE, 3000);
    fake_time_advance_ms(60 * 60 * 1000);

    fall_back_to_normal("Test");

    deye_ctrl_status_t st;
    deye_ctrl_get_status(&st);
    CHECK_I(st.mode, DEYE_MODE_NORMAL);
    CHECK_I(st.age_s, 0);                   /* not an hour old */
}

int main(void)
{
    RUN(test_normal_writes_the_reset_values);
    RUN(test_discharge_writes_mode_and_power);
    RUN(test_charge_converts_watt_to_ampere);
    RUN(test_a_silently_ignored_write_is_caught);
    RUN(test_a_failing_bus_is_caught);
    RUN(test_apply_clamps_and_remembers);
    RUN(test_apply_persists_so_a_restart_can_undo_it);
    RUN(test_the_guard_never_touches_the_user_setpoint);
    RUN(test_status_counts_down_and_stops);
    RUN(test_start_undoes_a_stored_forced_mode);
    RUN(test_start_is_quiet_when_nothing_was_forced);
    RUN(test_fallback_clears_the_store_only_once_confirmed);
    RUN(test_fallback_restarts_the_clock);
    return t_report("deye_ctrl");
}

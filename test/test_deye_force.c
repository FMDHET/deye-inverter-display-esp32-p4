#include "harness.h"
#include "fakes.h"

/* The Deye's forced mode by hand: it ends at the SoC (both directions), at
 * the time, and when someone else takes the Deye over -- and in the last
 * case it must not write anything back. deye_ctrl and the live values are
 * faked: what matters is what deye_force asks of them. */
#include "deye_force.c"

static deye_mode_t f_mode;
static int         f_user_w, f_applies, f_auto_soc;
static uint32_t    f_auto_limit;
static esp_err_t   f_auto_rc;
deye_mode_t deye_ctrl_get_mode(void) { return f_mode; }
int  deye_ctrl_get_user_power(void) { return f_user_w; }
esp_err_t deye_ctrl_apply(deye_mode_t m, int w) { f_mode = m; f_user_w = w; f_applies++; return ESP_OK; }
esp_err_t deye_ctrl_apply_auto(deye_mode_t m, int w, int soc, uint32_t lim)
{
    if (f_auto_rc != ESP_OK) return f_auto_rc;
    f_mode = m; f_user_w = w; f_auto_soc = soc; f_auto_limit = lim; f_applies++;
    return ESP_OK;
}
static mb_deye_live_t f_live;
void modbus_rtu_get_deye_live(mb_deye_live_t *o) { *o = f_live; }

#define T0 1792700000LL

static void fresh(void)
{
    memset(&s_f, 0, sizeof(s_f));
    f_mode = DEYE_MODE_NORMAL; f_user_w = 3000; f_applies = 0; f_auto_rc = ESP_OK;
    memset(&f_live, 0, sizeof(f_live));
    f_live.valid = f_live.online = true; f_live.bat_soc = 40;
}
static deye_force_t get(void) { deye_force_t f; deye_force_get(&f); return f; }

static void test_charge_to_soc(void)
{
    fresh();
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 4000, 90, 80, T0), ESP_OK);
    CHECK_I(f_mode, DEYE_MODE_FORCE_CHARGE);
    CHECK_I(f_auto_soc, 80);                   /* target into the inverter */
    CHECK_I(f_auto_limit, 90 * 60);            /* the time into deye_ctrl  */
    CHECK(get().active);
    f_live.bat_soc = 79.5f;
    deye_force_tick(T0 + 600);
    CHECK_I(get().mode, DEYE_FORCE_CHARGE);
    f_live.bat_soc = 80.0f;
    deye_force_tick(T0 + 610);
    CHECK_I(get().mode, DEYE_FORCE_OFF);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
    CHECK(strcmp(get().end, "Ziel-SoC erreicht") == 0);
}

static void test_discharge_floor_watched_here(void)
{
    fresh();
    f_live.bat_soc = 60;
    CHECK_I(deye_force_set(DEYE_FORCE_DISCHARGE, 5000, 60, 30, T0), ESP_OK);
    CHECK_I(f_auto_soc, 100);                  /* no floor register: not 30 */
    f_live.bat_soc = 31;
    deye_force_tick(T0 + 10);
    CHECK_I(f_mode, DEYE_MODE_FORCE_DISCHARGE);
    f_live.bat_soc = 30;
    deye_force_tick(T0 + 20);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
    /* no live values: no decision on the SoC */
    fresh();
    deye_force_set(DEYE_FORCE_DISCHARGE, 5000, 60, 30, T0);
    f_live.online = false; f_live.bat_soc = 5;
    deye_force_tick(T0 + 10);
    CHECK_I(f_mode, DEYE_MODE_FORCE_DISCHARGE);
}

static void test_time_and_takeover(void)
{
    fresh();
    deye_force_set(DEYE_FORCE_CHARGE, 3000, 30, 100, T0);
    deye_force_tick(T0 + 30 * 60);
    CHECK_I(get().mode, DEYE_FORCE_OFF);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
    CHECK(strcmp(get().end, "Zeit abgelaufen") == 0);

    /* someone sets another mode at the display: let go, write nothing */
    fresh();
    deye_force_set(DEYE_FORCE_CHARGE, 3000, 60, 100, T0);
    f_mode = DEYE_MODE_FORCE_DISCHARGE; f_user_w = 2000;
    int n = f_applies;
    deye_force_tick(T0 + 100);
    CHECK_I(get().mode, DEYE_FORCE_OFF);
    CHECK_I(f_applies, n);
    CHECK_I(f_mode, DEYE_MODE_FORCE_DISCHARGE);
    CHECK(strstr(get().end, "geändert") != NULL);

    /* deye_ctrl's own fallback at the end came first: "Zeit abgelaufen" */
    fresh();
    deye_force_set(DEYE_FORCE_CHARGE, 3000, 30, 100, T0);
    f_mode = DEYE_MODE_NORMAL;
    deye_force_tick(T0 + 30 * 60 - 5);
    CHECK(strcmp(get().end, "Zeit abgelaufen") == 0);

    /* off by hand */
    fresh();
    deye_force_set(DEYE_FORCE_DISCHARGE, 3000, 30, 20, T0);
    CHECK_I(deye_force_set(DEYE_FORCE_OFF, 0, 0, 0, T0 + 5), ESP_OK);
    CHECK_I(f_mode, DEYE_MODE_NORMAL);
    CHECK(strcmp(get().end, "von Hand beendet") == 0);
}

static void test_args(void)
{
    fresh();
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 100, 30, 80, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 3000, DEYE_FORCE_MAX_MIN + 1, 80, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 3000, 30, 5, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 3000, 30, 80, 1000), ESP_ERR_INVALID_STATE);
    CHECK_I(deye_force_set((deye_force_mode_t)7, 3000, 30, 80, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(f_applies, 0);
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 3000, 0, 80, T0), ESP_OK);    /* 0 = 60 min */
    CHECK_I(get().until, T0 + 3600);
    /* deye_ctrl refuses: no session */
    fresh();
    f_auto_rc = ESP_FAIL;
    CHECK_I(deye_force_set(DEYE_FORCE_CHARGE, 3000, 30, 80, T0), ESP_FAIL);
    CHECK_I(get().mode, DEYE_FORCE_OFF);
}

int main(void)
{
    RUN(test_charge_to_soc);
    RUN(test_discharge_floor_watched_here);
    RUN(test_time_and_takeover);
    RUN(test_args);
    return t_report("deye_force");
}

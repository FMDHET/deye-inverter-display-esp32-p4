#include "harness.h"
#include "fakes.h"

/* The energy manager's settings come from a web form and from MQTT. What
 * matters: nothing out of range gets stored, the Tibber token can only be
 * set on purpose, and a stored record survives a restart. */
#include "em_cfg.c"

static em_cfg_t base(void)
{
    em_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.lat = 48.80912f; c.lon = 9.27697f;
    c.nplanes = 2;
    snprintf(c.plane[0].name, sizeof(c.plane[0].name), "Ost");
    c.plane[0].kwp = 4.5f; c.plane[0].tilt = 30; c.plane[0].azimuth = 90;  c.plane[0].meter = 1;
    snprintf(c.plane[1].name, sizeof(c.plane[1].name), "Süd");
    c.plane[1].kwp = 5.2f; c.plane[1].tilt = 35; c.plane[1].azimuth = 180; c.plane[1].meter = 3;
    c.deye_kwh = 30.0f; c.byd_kwh = 7.7f; c.byd_reserve_pct = 10;
    c.charge_kw = 3.0f; c.min_spread_ct = 3.0f; c.cycle_cost_ct = 2.0f;
    c.soc_min_pct = 15; c.soc_max_pct = 95; c.mode = EM_MODE_SHADOW;
    return c;
}

static void reset(void)
{
    fake_nvs_reset();
    fake_nvs_reset_em();
    memset(&s_cfg, 0, sizeof(s_cfg));
}

static void test_a_good_record_is_stored_and_loaded(void)
{
    reset();
    em_cfg_t c = base();
    c.plane[1].ac_w = 5000;
    char err[100] = "";
    CHECK_I(em_cfg_set(&c, err, sizeof(err)), ESP_OK);
    memset(&s_cfg, 0, sizeof(s_cfg));
    em_cfg_init();                               /* the restart */
    em_cfg_t back;
    em_cfg_get(&back);
    CHECK_I(back.nplanes, 2);
    CHECK(strcmp(back.plane[1].name, "Süd") == 0);
    CHECK_F(back.plane[1].kwp, 5.2, 1e-5);
    CHECK_I(back.plane[0].azimuth, 90);
    CHECK_I(back.plane[1].ac_w, 5000);
    CHECK_I(back.plane[0].ac_w, 0);              /* none */
    CHECK_I(back.mode, EM_MODE_SHADOW);
    CHECK_F(back.lat, 48.80912, 1e-4);
}

static void test_out_of_range_is_refused_and_changes_nothing(void)
{
    reset();
    em_cfg_t good = base();
    char err[100];
    em_cfg_set(&good, err, sizeof(err));

    for (int i = 0; i < 14; i++) {                /* one bad field each */
        em_cfg_t c = base();
        switch (i) {
        case 0:  c.plane[0].kwp = 0.0f; break;
        case 1:  c.plane[0].tilt = 91; break;
        case 2:  c.plane[0].azimuth = 360; break;
        case 3:  c.plane[0].meter = 8; break;
        case 4:  c.lat = 95.0f; break;
        case 5:  c.deye_kwh = -1.0f; break;
        case 6:  c.byd_reserve_pct = 101; break;
        case 7:  c.charge_kw = 25.0f; break;
        case 8:  c.soc_min_pct = 90; c.soc_max_pct = 95; break;
        case 9:  c.mode = 3; break;
        case 10: c.plane[1].kwp = NAN; break;
        case 11: c.nplanes = EM_MAX_PLANES + 1; break;
        case 12: c.plane[0].ac_w = EM_PLANE_AC_MAX_W + 1; break;
        case 13: c.byd_max_soc = 30; break;           /* below 50 %: a typo, not a limit */
        }
        err[0] = '\0';
        CHECK(em_cfg_set(&c, err, sizeof(err)) != ESP_OK);
        CHECK(err[0] != '\0');                   /* says why */
        em_cfg_t now;
        em_cfg_get(&now);
        CHECK(memcmp(&now, &s_cfg, sizeof(now)) == 0);
        CHECK_F(now.plane[0].kwp, 4.5, 1e-5);    /* the good record stands */
    }
}

static void test_names_are_tidied(void)
{
    reset();
    em_cfg_t c = base();
    snprintf(c.plane[0].name, sizeof(c.plane[0].name), "a\"b\\c\nd");
    char err[100];
    CHECK_I(em_cfg_set(&c, err, sizeof(err)), ESP_OK);
    em_cfg_get(&c);
    CHECK(strcmp(c.plane[0].name, "a b c d") == 0);   /* nothing that breaks JSON */
}

static void test_the_token_only_changes_on_purpose(void)
{
    reset();
    char err[100];
    em_cfg_t c = base();
    em_cfg_set(&c, err, sizeof(err));
    CHECK(!em_cfg_has_token());
    CHECK_I(em_cfg_set_token("abc-DEF_123.x", err, sizeof(err)), ESP_OK);
    CHECK(em_cfg_has_token());

    /* A settings save does not touch it, even with garbage in the field. */
    c = base();
    snprintf(c.tibber_token, sizeof(c.tibber_token), "overwritten");
    em_cfg_set(&c, err, sizeof(err));
    em_cfg_get(&c);
    CHECK(strcmp(c.tibber_token, "abc-DEF_123.x") == 0);

    CHECK(em_cfg_set_token("with space", err, sizeof(err)) != ESP_OK);
    CHECK(em_cfg_set_token("quote\"", err, sizeof(err)) != ESP_OK);
    em_cfg_get(&c);
    CHECK(strcmp(c.tibber_token, "abc-DEF_123.x") == 0);

    CHECK_I(em_cfg_set_token("", err, sizeof(err)), ESP_OK);   /* delete */
    CHECK(!em_cfg_has_token());
}

static void test_defaults_for_an_old_record(void)
{
    reset();
    em_cfg_t c = base();
    c.charge_kw = 0.0f; c.min_spread_ct = 0.0f; c.soc_max_pct = 0; c.soc_min_pct = 0;
    char err[100];
    CHECK_I(em_cfg_set(&c, err, sizeof(err)), ESP_OK);
    em_cfg_get(&c);
    CHECK_F(c.charge_kw, EM_DEF_CHARGE_KW, 1e-6);
    CHECK_F(c.min_spread_ct, EM_DEF_MIN_SPREAD, 1e-6);
    CHECK_I(c.soc_max_pct, EM_DEF_SOC_MAX);
}

static void test_a_failed_write_keeps_the_old_settings(void)
{
    reset();
    char err[100];
    em_cfg_t c = base();
    em_cfg_set(&c, err, sizeof(err));
    c.deye_kwh = 12.0f;
    fake_nvs_fail = true;
    CHECK(em_cfg_set(&c, err, sizeof(err)) != ESP_OK);
    fake_nvs_fail = false;
    em_cfg_get(&c);
    CHECK_F(c.deye_kwh, 30.0, 1e-6);
}

static void test_the_byd_charge_cap(void)
{
    reset();
    char err[100];
    em_cfg_t c = base();
    c.byd_max_soc = 80;
    CHECK_I(em_cfg_set(&c, err, sizeof(err)), ESP_OK);
    em_cfg_t now;
    em_cfg_get(&now);
    CHECK_I(now.byd_max_soc, 80);
    c.byd_max_soc = 100;                          /* 100 % = no limit = 0 */
    CHECK_I(em_cfg_set(&c, err, sizeof(err)), ESP_OK);
    em_cfg_get(&now);
    CHECK_I(now.byd_max_soc, 0);
    c.byd_max_soc = 50;
    CHECK_I(em_cfg_set(&c, err, sizeof(err)), ESP_OK);
    em_cfg_get(&now);
    CHECK_I(now.byd_max_soc, 50);
}

int main(void)
{
    RUN(test_a_good_record_is_stored_and_loaded);
    RUN(test_out_of_range_is_refused_and_changes_nothing);
    RUN(test_names_are_tidied);
    RUN(test_the_token_only_changes_on_purpose);
    RUN(test_the_byd_charge_cap);
    RUN(test_defaults_for_an_old_record);
    RUN(test_a_failed_write_keeps_the_old_settings);
    return t_report("em_cfg");
}

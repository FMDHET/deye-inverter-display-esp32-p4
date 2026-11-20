#include "harness.h"

/* Load vs. temperature: only a clear heating relation may scale the load
 * forecast; a house without electric heating must keep factor 1. */
#define LW_HOST_TEST
#include "../main/load_weather.c"

static void test_heat_pump_house(void)
{
    /* ratio = 0.7 + 0.04 * HDD with a little noise, 30 days from 0 to 12 HDD */
    lw_point_t p[30];
    for (int i = 0; i < 30; i++) {
        p[i].hdd = (float)(i % 13);
        p[i].ratio = 0.7f + 0.04f * p[i].hdd + ((i * 7) % 5 - 2) * 0.01f;
    }
    lw_model_t m;
    lw_fit(p, 30, &m);
    CHECK(m.ok);
    CHECK_F(m.beta, 0.04, 0.005);
    CHECK_F(m.alpha, 0.7, 0.03);
    CHECK(m.r2 > 0.8f);
    CHECK_F(lw_factor(&m, 15.0f), m.alpha, 1e-6);              /* no heating */
    CHECK_F(lw_factor(&m, -5.0f), m.alpha + 20 * m.beta, 1e-5); /* 20 HDD */
    CHECK_F(lw_factor(&m, -60.0f), LW_FACTOR_MAX, 1e-6);         /* clamped */
    CHECK_F(lw_factor(&m, NAN), 1.0f, 0);
}

static void test_no_heating_keeps_one(void)
{
    lw_point_t p[30];
    for (int i = 0; i < 30; i++) { p[i].hdd = (float)(i % 13); p[i].ratio = 1.0f + ((i * 7) % 5 - 2) * 0.05f; }
    lw_model_t m;
    lw_fit(p, 30, &m);
    CHECK(!m.ok);
    CHECK_F(lw_factor(&m, -10.0f), 1.0f, 0);
}

static void test_too_few_days_or_flat_weather(void)
{
    lw_point_t p[30];
    for (int i = 0; i < 10; i++) { p[i].hdd = (float)i; p[i].ratio = 0.8f + 0.05f * i; }
    lw_model_t m;
    lw_fit(p, 10, &m);
    CHECK(!m.ok);                                   /* clear, but only 10 days */
    for (int i = 0; i < 30; i++) { p[i].hdd = 5.0f + (i % 3) * 0.5f; p[i].ratio = 0.8f + 0.05f * p[i].hdd; }
    lw_fit(p, 30, &m);
    CHECK(!m.ok);                                   /* spread of 0.4 K says nothing */
    lw_fit(p, 0, &m);
    CHECK(!m.ok);
}

int main(void)
{
    RUN(test_heat_pump_house);
    RUN(test_no_heating_keeps_one);
    RUN(test_too_few_days_or_flat_weather);
    return t_report("load_weather");
}

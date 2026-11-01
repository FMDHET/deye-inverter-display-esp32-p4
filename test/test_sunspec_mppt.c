#include "harness.h"

/* What counts as PV in a SunSpec MPPT model. The numbers that went wrong on
 * the real Symo Hybrid (2026-10-24, before sunrise): String 1 4 W (PV, dark),
 * String 2 1989 W (the BYD discharging) -- summed as "PV 1993 W". */
#include "sunspec_mppt.c"

static void test_symo_hybrid(void)
{
    const char ids[2][17] = { "String 1", "String 2" };
    CHECK_I(sunspec_mppt_pv_mask(ids, 2, true), 0x1);     /* battery on the last input */
    CHECK_I(sunspec_mppt_pv_mask(ids, 2, false), 0x3);    /* no storage: both strings */
}

static void test_gen24(void)
{
    const char ids[4][17] = { "MPPT 1", "MPPT 2", "StCha 3", "StDisCha 4" };
    CHECK_I(sunspec_mppt_pv_mask(ids, 4, true), 0x3);     /* named: by name, not position */
    CHECK_I(sunspec_mppt_pv_mask(ids, 4, false), 0x3);
}

static void test_plain_inverters(void)
{
    const char one[1][17] = { "String 1" };
    CHECK_I(sunspec_mppt_pv_mask(one, 1, false), 0x1);
    CHECK_I(sunspec_mppt_pv_mask(one, 1, true), 0x1);     /* one input: nothing to take away */
    const char none[1][17] = { "" };
    CHECK_I(sunspec_mppt_pv_mask(none, 0, true), 0x0);
    char many[20][17];
    for (int i = 0; i < 20; i++) snprintf(many[i], 17, "S%d", i);
    CHECK_I(sunspec_mppt_pv_mask((const char (*)[17])many, 20, false), 0xFFFF);   /* clamped to 16 */
}

int main(void)
{
    RUN(test_symo_hybrid);
    RUN(test_gen24);
    RUN(test_plain_inverters);
    return t_report("sunspec_mppt");
}

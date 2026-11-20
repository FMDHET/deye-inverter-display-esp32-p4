#include "harness.h"

/* What the energy in the batteries cost. Efficiencies are the Deye's measured
 * long-term values of 2026-11-20 (charge 406.7/437.0, discharge 441.3/492.3);
 * 11 ct feed-in as the operator's example. */
#include "bat_cost_calc.h"

#define EC 0.9306f
#define ED 0.8964f
#define FEED 11.0

static bc_bat_t bat(float content, float dc_w) { bc_bat_t b = { content, dc_w, EC, ED }; return b; }
static const bc_bat_t NONE = { NAN, NAN, EC, ED };

static void test_start_value_is_feed_in_with_losses(void)
{
    bc_pack_t p;
    bc_pack_init(&p, FEED, EC);
    CHECK_F(bc_out_ct(&p, ED), 11.0 / EC / ED, 1e-5);
    CHECK_F(bc_out_ct(&p, ED), 13.19, 0.01);         /* the number on the display */
    CHECK_F(p.grid, 0.0, 1e-12);
}

static void test_pv_charge_keeps_pv_price(void)
{
    bc_pack_t p, q;
    bc_pack_init(&p, FEED, EC);
    bc_pack_init(&q, FEED, EC);
    bc_bat_t a = bat(20.0f, -5000.0f);
    bc_bat_t b = NONE;
    bc_stats_t s = {0};
    bc_step(&p, &q, &a, &b, -2000.0, 30.0, FEED, 1.0, &s, NULL);   /* exporting: all PV */
    CHECK_F(p.ct, FEED / EC, 1e-5);
    CHECK_F(p.grid, 0.0, 1e-12);
    CHECK_F(s.pv_kwh, 5.0 / EC, 1e-5);
    CHECK_F(s.grid_kwh, 0.0, 1e-12);
    CHECK_F(s.cost_ct, 5.0 / EC * FEED, 1e-4);
}

static void test_grid_into_empty_battery(void)
{
    bc_pack_t p, q;
    bc_pack_init(&p, FEED, EC);
    bc_pack_init(&q, FEED, EC);
    bc_bat_t a = bat(0.0f, -3000.0f), b = NONE;
    bc_step(&p, &q, &a, &b, 8000.0, 30.0, FEED, 1.0, NULL, NULL);
    CHECK_F(bc_out_ct(&p, ED), 30.0 / EC / ED, 1e-9);
    CHECK_F(p.grid, 1.0, 1e-9);
}

static void test_half_grid_half_pv_content(void)
{
    /* 10 kWh of solar in the cells, then 10 kWh more (cell side) from the grid
     * at 30 ct: the pot holds half of each. */
    bc_pack_t p, q;
    bc_pack_init(&p, FEED, EC);
    bc_pack_init(&q, FEED, EC);
    bc_bat_t a = bat(10.0f, -10000.0f), b = NONE;
    bc_step(&p, &q, &a, &b, 12000.0, 30.0, FEED, 1.0, NULL, NULL);
    CHECK_F(p.ct, (11.0 + 30.0) / 2 / EC, 1e-6);
    CHECK_F(p.grid, 0.5, 1e-6);
}

static void test_discharge_keeps_price(void)
{
    bc_pack_t p = { 20.0, 0.4 }, q;
    bc_pack_init(&q, FEED, EC);
    bc_bat_t a = bat(30.0f, 4000.0f), b = NONE;
    bc_step(&p, &q, &a, &b, 1000.0, 45.0, FEED, 1.0, NULL, NULL);
    CHECK_F(p.ct, 20.0, 1e-12);
    CHECK_F(p.grid, 0.4, 1e-12);
}

static void test_house_is_fed_first(void)
{
    /* 3 kW charge (AC), 1 kW import: PV covers the house and 2 kW of the
     * charge, the grid the rest. 5 kW import: all of the charge is grid. */
    bc_src_t a, b;
    bc_split(3000, 0, 0, 0, 1000, &a, &b);
    CHECK_F(a.grid_w, 1000, 1e-9);
    CHECK_F(a.pv_w, 2000, 1e-9);
    bc_split(3000, 0, 0, 0, 5000, &a, &b);
    CHECK_F(a.grid_w, 3000, 1e-9);
    CHECK_F(a.pv_w, 0, 1e-9);
    bc_split(3000, 0, 0, 0, 0, &a, &b);
    CHECK_F(a.pv_w, 3000, 1e-9);
}

static void test_two_batteries_share_import(void)
{
    bc_src_t a, b;
    bc_split(3000, 1000, 0, 0, 2000, &a, &b);
    CHECK_F(a.grid_w, 1500, 1e-9);
    CHECK_F(b.grid_w, 500, 1e-9);
    CHECK_F(a.pv_w, 1500, 1e-9);
    CHECK_F(b.pv_w, 500, 1e-9);
}

static void test_byd_feeds_deye(void)
{
    /* The BYD (b) discharges 2 kW AC while the Deye (a) charges 3 kW AC with
     * 0.5 kW import: 0.5 grid, 2 BYD, 0.5 PV. The BYD's energy arrives at its
     * own output price and brings its grid share along. */
    bc_src_t sa, sb;
    bc_split(3000, 0, 0, 2000, 500, &sa, &sb);
    CHECK_F(sa.grid_w, 500, 1e-9);
    CHECK_F(sa.other_w, 2000, 1e-9);
    CHECK_F(sa.pv_w, 500, 1e-9);

    bc_pack_t deye = { 0.0, 0.0 }, byd = { 20.0, 0.5 };
    bc_bat_t a = bat(0.0f, -3000.0f * EC);           /* 3 kW on the AC side */
    bc_bat_t b = { 5.0f, 2000.0f / 0.95f, 0.95f, 0.95f };   /* delivers 2 kW AC */
    bc_step(&deye, &byd, &a, &b, 500.0, 30.0, FEED, 1.0, NULL, NULL);
    double byd_out = 20.0 / 0.95;
    double cost = 0.5 * 30.0 + 2.0 * byd_out + 0.5 * FEED;
    CHECK_F(deye.ct, cost / (3.0 * EC), 1e-6);
    CHECK_F(deye.grid, (0.5 + 2.0 * 0.5) / 3.0, 1e-6);
    CHECK_F(byd.ct, 20.0, 1e-12);                    /* discharging: unchanged */
}

static void test_idle_and_unknown_change_nothing(void)
{
    bc_pack_t p = { 12.0, 0.1 }, q = { 14.0, 0.2 };
    bc_bat_t a = bat(10.0f, -30.0f), b = bat(3.0f, -45.0f);    /* standby draw */
    bc_step(&p, &q, &a, &b, 3000.0, 40.0, FEED, 10.0, NULL, NULL);
    CHECK_F(p.ct, 12.0, 1e-12);
    CHECK_F(q.ct, 14.0, 1e-12);

    bc_bat_t u = { NAN, -3000.0f, EC, ED };                    /* SoC unknown */
    bc_step(&p, &q, &u, &b, 3000.0, 40.0, FEED, 1.0, NULL, NULL);
    CHECK_F(p.ct, 12.0, 1e-12);
    CHECK_F(p.grid, 0.1, 1e-12);
}

static void test_short_steps_add_up(void)
{
    /* 360 steps of 10 s at 3.6 kW == one step of an hour */
    bc_pack_t p1 = { 15.0, 0.2 }, p2 = { 15.0, 0.2 }, q;
    bc_pack_init(&q, FEED, EC);
    bc_bat_t b = NONE;
    bc_bat_t one = bat(20.0f, -3600.0f);
    bc_step(&p1, &q, &one, &b, 1800.0, 30.0, FEED, 1.0, NULL, NULL);
    for (int i = 0; i < 360; i++) {
        bc_bat_t s = bat(20.0f + 3.6f * i / 360.0f, -3600.0f)   /* the cells fill */;
        bc_step(&p2, &q, &s, &b, 1800.0, 30.0, FEED, 10.0 / 3600.0, NULL, NULL);
    }
    CHECK_F(p2.ct, p1.ct, 0.01);
    CHECK_F(p2.grid, p1.grid, 0.001);
}

int main(void)
{
    RUN(test_start_value_is_feed_in_with_losses);
    RUN(test_pv_charge_keeps_pv_price);
    RUN(test_grid_into_empty_battery);
    RUN(test_half_grid_half_pv_content);
    RUN(test_discharge_keeps_price);
    RUN(test_house_is_fed_first);
    RUN(test_two_batteries_share_import);
    RUN(test_byd_feeds_deye);
    RUN(test_idle_and_unknown_change_nothing);
    RUN(test_short_steps_add_up);
    return t_report("bat_cost");
}

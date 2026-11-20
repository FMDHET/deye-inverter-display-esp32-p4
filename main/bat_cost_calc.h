#pragma once

/* What the energy sitting in a battery cost -- the arithmetic, pure and
 * host-tested (test/test_bat_cost.c). The glue around it is bat_cost.c.
 *
 * Model: a battery is a pot. Every charged kWh goes in with a price, every
 * discharged kWh comes out at the pot's average, which therefore does not
 * change on discharge. The pot's size is taken from the battery itself
 * (SoC x capacity) on every step, not integrated, so a counting error never
 * accumulates: an empty battery forgets its history, a full one dilutes new
 * energy accordingly.
 *
 * Prices are what the energy was WORTH where it came from (AC side):
 *   grid           the end price at that moment (Tibber)
 *   PV             the feed-in tariff -- that is what exporting it would have
 *                  paid, so it is what storing it costs
 *   other battery  that battery's own price per kWh it delivers
 * Losses are included: the pot holds cell energy (DC), charging buys
 * chg_ac x eta_c of it, discharging delivers cell x eta_d. The price shown
 * is per kWh that comes OUT on the AC side, i.e. pot price / eta_d.
 *
 * Who gets the grid: the house is fed first from PV and from discharging
 * batteries, so a charging battery takes the grid import first (up to its
 * charge power), then the other battery's discharge, then PV. Two batteries
 * charging at once share the import in proportion to their charge power. */

#include <math.h>
#include <stdbool.h>

/* Below this a battery counts as idle: standby draw and sensor offset
 * would otherwise trickle grid-priced energy into the pot all night. */
#define BC_IDLE_W  50.0f

typedef struct {
    double ct;     /* price of one kWh in the cells (DC side), ct             */
    double grid;   /* share of the cell energy that came from the grid, 0..1  */
} bc_pack_t;

/* Running totals since the last reset, per battery (AC side, kWh / ct). */
typedef struct {
    double pv_kwh, grid_kwh, other_kwh;
    double cost_ct;
} bc_stats_t;

/* One battery for one step. */
typedef struct {
    float content_kwh;   /* cell energy before the step (SoC x capacity); NAN unknown */
    float dc_w;          /* cell power: + discharge, - charge; NAN unknown            */
    float eta_c, eta_d;  /* AC -> cells and cells -> AC, 0 < eta <= 1                 */
} bc_bat_t;

/* Price per kWh a pack delivers on the AC side. */
static inline double bc_out_ct(const bc_pack_t *p, float eta_d)
{
    return eta_d > 0.0f ? p->ct / eta_d : p->ct;
}

/* The pot of a battery nothing is known about yet: assume solar, as the
 * operator chose -- the content is priced at the feed-in tariff. */
static inline void bc_pack_init(bc_pack_t *p, float feedin_ct, float eta_c)
{
    p->ct   = eta_c > 0.0f ? feedin_ct / eta_c : feedin_ct;
    p->grid = 0.0;
}

/* AC watts each source puts into a charging battery. */
typedef struct { double grid_w, other_w, pv_w; } bc_src_t;

/* Split the AC charge powers of two batteries (a, b) over grid, the other
 * battery and PV. chg/dis are AC watts >= 0; import_w is the grid import
 * (>= 0, export counts as 0). */
static inline void bc_split(double a_chg, double b_chg, double a_dis, double b_dis,
                            double import_w, bc_src_t *a, bc_src_t *b)
{
    double c = a_chg + b_chg;
    double g = import_w < c ? import_w : c;
    if (g < 0) g = 0;
    a->grid_w = c > 0 ? g * a_chg / c : 0;
    b->grid_w = c > 0 ? g * b_chg / c : 0;
    double ra = a_chg - a->grid_w, rb = b_chg - b->grid_w;
    a->other_w = ra < b_dis ? ra : b_dis;
    b->other_w = rb < a_dis ? rb : a_dis;
    if (a->other_w < 0) a->other_w = 0;
    if (b->other_w < 0) b->other_w = 0;
    a->pv_w = ra - a->other_w;
    b->pv_w = rb - b->other_w;
}

/* Put chg_ac_kwh (AC) worth cost_ct into a pot that held content_kwh of cell
 * energy; grid_ac_kwh of it counts as grid energy. */
static inline void bc_charge(bc_pack_t *p, double content_kwh, double chg_ac_kwh, float eta_c,
                             double cost_ct, double grid_ac_kwh)
{
    double add = chg_ac_kwh * eta_c;
    double e   = content_kwh > 0 ? content_kwh : 0;
    if (add <= 0 || e + add < 1e-9) return;
    p->ct   = (p->ct * e + cost_ct) / (e + add);
    p->grid = (p->grid * e + grid_ac_kwh * eta_c) / (e + add);
    if (p->grid < 0) p->grid = 0;
    if (p->grid > 1) p->grid = 1;
}

static inline bool bc_bat_known(const bc_bat_t *b)
{
    return isfinite(b->content_kwh) && isfinite(b->dc_w) && b->eta_c > 0.0f && b->eta_d > 0.0f;
}

/* AC watts a battery draws while charging / delivers while discharging. */
static inline double bc_chg_ac(const bc_bat_t *b)
{
    return (isfinite(b->dc_w) && b->dc_w < -BC_IDLE_W && b->eta_c > 0) ? -b->dc_w / b->eta_c : 0;
}
static inline double bc_dis_ac(const bc_bat_t *b)
{
    return (isfinite(b->dc_w) && b->dc_w > BC_IDLE_W) ? b->dc_w * b->eta_d : 0;
}

/* One step of dt_h hours for both batteries. A battery whose content or power
 * is unknown is left alone (its pot is still used as a source when it is
 * known to discharge). import_w must be known (>= 0 or export < 0): without
 * the grid there is no telling where the energy came from, so the caller
 * skips the step. */
static inline void bc_step(bc_pack_t *pa, bc_pack_t *pb, const bc_bat_t *a, const bc_bat_t *b,
                           double grid_w, double grid_ct, double feedin_ct, double dt_h,
                           bc_stats_t *sa, bc_stats_t *sb)
{
    double imp = grid_w > 0 ? grid_w : 0;
    bool ka = bc_bat_known(a), kb = bc_bat_known(b);
    double a_chg = ka ? bc_chg_ac(a) : 0, b_chg = kb ? bc_chg_ac(b) : 0;
    double a_dis = isfinite(a->dc_w) ? bc_dis_ac(a) : 0, b_dis = isfinite(b->dc_w) ? bc_dis_ac(b) : 0;
    bc_src_t sra, srb;
    bc_split(a_chg, b_chg, a_dis, b_dis, imp, &sra, &srb);

    /* prices of the other battery's output, taken before either pot moves */
    double a_out = bc_out_ct(pa, a->eta_d), b_out = bc_out_ct(pb, b->eta_d);
    double a_gr = pa->grid, b_gr = pb->grid;

    if (a_chg > 0) {
        double g = sra.grid_w * dt_h / 1000, o = sra.other_w * dt_h / 1000, v = sra.pv_w * dt_h / 1000;
        double cost = g * grid_ct + o * b_out + v * feedin_ct;
        bc_charge(pa, a->content_kwh, g + o + v, a->eta_c, cost, g + o * b_gr);
        if (sa) { sa->grid_kwh += g; sa->other_kwh += o; sa->pv_kwh += v; sa->cost_ct += cost; }
    }
    if (b_chg > 0) {
        double g = srb.grid_w * dt_h / 1000, o = srb.other_w * dt_h / 1000, v = srb.pv_w * dt_h / 1000;
        double cost = g * grid_ct + o * a_out + v * feedin_ct;
        bc_charge(pb, b->content_kwh, g + o + v, b->eta_c, cost, g + o * a_gr);
        if (sb) { sb->grid_kwh += g; sb->other_kwh += o; sb->pv_kwh += v; sb->cost_ct += cost; }
    }
}

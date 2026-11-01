/* Energy manager planner -- see em_planner.h for the contract.
 *
 * Structure:
 *   slot_step()   one quarter hour of the battery: limits, losses, grid, cost
 *   run()         the whole horizon for the current decisions (the "base")
 *   delta()       the same horizon with ONE slot changed, as a cost difference
 *                 against the base -- the inner loop of the greedy
 *   greedy()      pick the most expensive importing slot, try every cheaper
 *                 earlier slot as HOLD or as one more charge quantum, accept
 *                 the best move that pays, repeat
 *   release()     take back holds and charge quanta that no longer pay
 *   em_plan_compute() alternates the two a few rounds
 *
 * All arithmetic is float on purpose (single-precision FPU on the P4). */

#include "em_planner.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>


#define DT_H        ((float)EM_SLOT_S / 3600.0f)   /* 0.25 h                   */
#define CHG_Q_KW    (EM_PLAN_CHG_QUANTUM_KWH / DT_H)
/* A slot "imports" above this. Below it is meter-noise territory, and chasing
 * a 10 W import with a battery move is not worth a pass. */
#define IMPORT_EPS_KW 0.01f
/* Two trajectories whose battery energy differs by less than this are the
 * same trajectory: from there on the rest of the horizon is identical, so
 * delta() can stop. 0.01 Wh is far below anything that changes a cost. */
#define SAME_E_KWH  1e-5f
/* Smallest saving a move must bring on top of the spread, in ct. Guards
 * against accepting float noise as a "saving" when min_spread is 0. */
#define MIN_GAIN_CT 0.01f
/* Smallest energy a move must actually move to count as one. */
#define MIN_MOVE_KWH 1e-4f

#define TERM_N 129            /* points of the end-value table              */

typedef struct {
    float e0;                 /* DC kWh in the battery now                 */
    float e_min;              /* discharge floor                           */
    float e_gmax;             /* grid-charge ceiling                       */
    float e_full;             /* PV may fill up to here                    */
    float dis_max, chg_max;   /* AC kW                                     */
    float ld[3], lc[3];       /* loss coefficients, discharge / charge     */
    float spread, cycle, feedin;
    float pv_scale;
    float term_per_kwh;       /* value of one DC kWh left at the end       */
    /* With slots after the horizon (in->n_ext): the value of the energy left
     * at the end, tabulated over e_min..e_full (see ctx_init). */
    bool  term_tab_ok;
    float term_step;
    float term_tab[TERM_N];
    float rt_max;             /* best round-trip efficiency of the model   */
    bool  battery;            /* false: nothing to plan                    */
} ctx_t;

typedef struct {
    float grid;               /* kW, + import                              */
    float cost;               /* ct                                        */
    float p_dis;              /* AC kW delivered by the battery            */
    float g_used;             /* AC kW actually taken from the grid to charge */
} step_t;

/* What the battery accepts (DC kW) at energy e: full power up to
 * EM_TAPER_FROM_PCT, then linearly less down to EM_TAPER_END of it at 100 %
 * -- the constant-voltage end of a charge, where the BMS slows down
 * (operator, 2026-10-25: a plan that counts on full speed to the last
 * percent comes up short and buys the rest later, dearer). Full power =
 * the configured charge power; without one, no limit. */
static inline float accept_kw(const ctx_t *c, float e)
{
    if (c->chg_max <= 0.0f || c->e_full <= 0.0f) return INFINITY;
    float soc = 100.0f * e / c->e_full;
    if (soc <= EM_TAPER_FROM_PCT) return INFINITY;
    float x = (soc - EM_TAPER_FROM_PCT) / (100.0f - EM_TAPER_FROM_PCT);
    if (x > 1.0f) x = 1.0f;
    return c->chg_max * (1.0f - (1.0f - EM_TAPER_END) * x);
}

/* DC energy that goes in during one slot from energy e when `rate` DC kW is
 * offered: the full rate until the acceptance falls below it, then the
 * acceptance itself. Linear in e, so that part is an exponential approach
 * -- integrated exactly, not taken at the slot's start (a quarter hour at
 * 18 kW crosses the whole taper of a small battery). Room limits are the
 * caller's. */
static float dc_accepted(const ctx_t *c, float e, float rate)
{
    if (c->chg_max <= 0.0f || c->e_full <= 0.0f) return rate * DT_H;
    float e_t = c->e_full * EM_TAPER_FROM_PCT / 100.0f;
    float k   = c->chg_max * (1.0f - EM_TAPER_END) / (c->e_full - e_t);  /* 1/h */
    /* Where the acceptance meets the offered rate. */
    float e_r = rate >= c->chg_max ? e_t : e_t + (c->chg_max - rate) / k;
    float t = DT_H, got = 0.0f;
    if (e < e_r) {
        float t1 = (e_r - e) / rate;
        if (t1 >= t) return rate * t;
        got = e_r - e; t -= t1; e = e_r;
    }
    float u0 = accept_kw(c, e);
    if (!isfinite(u0)) u0 = c->chg_max;
    if (u0 > rate) u0 = rate;
    if (u0 <= 0.0f) return got;
    return got + u0 / k * (1.0f - expf(-k * t));
}

static inline float loss_kw(const float *l, float p)
{
    return l[0] + (l[1] + l[2] * p) * p;
}

/* Finite and non-negative, else the fallback. Inputs come from forecasts and
 * NVS; a NAN that slipped through must not poison every sum downstream. */
static inline float sane(float v, float fallback)
{
    return (isfinite(v) && v >= 0.0f) ? v : fallback;
}

/* One quarter hour. Returns the battery energy at the end of the slot.
 *
 * The three actions share one model and differ only in two switches: may the
 * battery discharge (NORMAL only), and is there a grid charge on top of the
 * PV surplus (CHARGE only). Keeping it one function is what keeps the base
 * run and every candidate evaluation consistent with each other. */
static float slot_step(const ctx_t *c, const em_plan_in_t *in, uint16_t i,
                       float e, uint8_t act, float g_req, step_t *o)
{
    float pv   = in->pv_kw[i] > 0.0f ? in->pv_kw[i] * c->pv_scale : 0.0f;
    float load = in->load_kw[i] > 0.0f ? in->load_kw[i] : 0.0f;
    float net  = pv - load;
    float sur  = net > 0.0f ? net : 0.0f;
    float def  = net < 0.0f ? -net : 0.0f;
    float g    = (act == EM_ACT_CHARGE && g_req > 0.0f) ? g_req : 0.0f;
    float pv_used = 0.0f, g_used = 0.0f, p_dis = 0.0f, dc_dis = 0.0f;

    /* Charging: PV surplus and grid charge go through the inverter together,
     * so the loss is taken at their SUM -- 1 kW of grid charge on top of
     * 3 kW of PV costs far less than 1 kW alone. The DC result is split back
     * in proportion so each source can meet its own ceiling: PV may fill to
     * 100 %, the grid only to soc_max. */
    float p_in = sur + g;
    if (p_in > 0.0f) {
        float rate = p_in - loss_kw(c->lc, p_in);
        if (rate > 0.0f) {
            float dc_pv_u = rate * DT_H * (sur / p_in);
            float dc_g_u  = rate * DT_H * (g / p_in);
            /* Saturation: near full the battery takes less (dc_accepted).
             * The cap scales both sources alike, before the room limits. */
            float f = dc_accepted(c, e, rate) / (rate * DT_H);
            if (f > 1.0f) f = 1.0f;
            float room    = c->e_full - e;
            float dc_pv   = dc_pv_u * f < room ? dc_pv_u * f : (room > 0.0f ? room : 0.0f);
            float groom   = c->e_gmax - e - dc_pv;
            float dc_g    = dc_g_u * f < groom ? dc_g_u * f : (groom > 0.0f ? groom : 0.0f);
            /* A capped slot takes proportionally less AC: the battery stops
             * part-way through the quarter hour at the same efficiency. */
            if (dc_pv_u > 0.0f) pv_used = sur * (dc_pv / dc_pv_u);
            if (dc_g_u  > 0.0f) g_used  = g   * (dc_g  / dc_g_u);
            e += dc_pv + dc_g;
        }
        /* rate <= 0: the trickle does not even cover the inverter's own
         * consumption. Nothing is stored; the surplus counts as exported. */
    }

    /* Discharging: only NORMAL, only for a deficit, at most dis_max, and only
     * what is above the floor. When the floor cuts in, solve
     * P + loss(P) = available/dt for the AC power that is still possible. */
    if (act == EM_ACT_NORMAL && def > 0.0f && c->dis_max > 0.0f) {
        float p    = def < c->dis_max ? def : c->dis_max;
        float need = (p + loss_kw(c->ld, p)) * DT_H;
        float avail = e - c->e_min;
        if (avail <= 0.0f) {
            p = 0.0f; need = 0.0f;
        } else if (need > avail) {
            float r = avail / DT_H - c->ld[0];
            if (r <= 0.0f) {
                p = 0.0f; need = 0.0f;   /* too little left to run at all */
            } else {
                float b1 = 1.0f + c->ld[1];
                p = c->ld[2] > 0.0f
                    ? (sqrtf(b1 * b1 + 4.0f * c->ld[2] * r) - b1) / (2.0f * c->ld[2])
                    : r / b1;
                need = avail;
            }
        }
        p_dis  = p;
        dc_dis = need;
        e -= need;
    }

    float grid = load - pv + pv_used + g_used - p_dis;
    o->grid   = grid;
    o->cost   = (grid > 0.0f ? grid * in->price_ct[i] : grid * c->feedin) * DT_H
              + c->cycle * dc_dis;
    o->p_dis  = p_dis;
    o->g_used = g_used;
    return e;
}

static inline float term_value(const ctx_t *c, float e)
{
    float x = e - c->e_min;
    if (x <= 0.0f) return 0.0f;
    if (!c->term_tab_ok) return x * c->term_per_kwh;
    float f = x / c->term_step;
    if (f >= (float)(TERM_N - 1)) return c->term_tab[TERM_N - 1];
    int   i = (int)f;
    return c->term_tab[i] + (f - (float)i) * (c->term_tab[i + 1] - c->term_tab[i]);
}

/* What one more DC kWh at the end is worth when the battery ends at e: the
 * slope of the end value. The greedy ranks the end among the dear slots by
 * it -- with the table it falls to ~0 where the next day's sun fills the
 * battery anyway. */
static inline float term_slope(const ctx_t *c, float e)
{
    if (!c->term_tab_ok) return c->term_per_kwh;
    float h = c->term_step;
    return (term_value(c, e + h) - term_value(c, e)) / h;
}

/* The whole horizon for the decisions in s[]. Writes grid_kw and cost_ct of
 * every slot and the energy trajectory e[0..n]; returns the total cost
 * including the terminal value. */
static float run(const ctx_t *c, const em_plan_in_t *in, em_slot_plan_t *s,
                 uint16_t n, float *e)
{
    float sum = 0.0f;
    e[0] = c->e0;
    for (uint16_t i = 0; i < n; i++) {
        step_t st;
        e[i + 1] = slot_step(c, in, i, e[i], s[i].act, s[i].charge_kw, &st);
        s[i].grid_kw = st.grid;
        s[i].cost_ct = st.cost;
        sum += st.cost;
    }
    return sum - term_value(c, e[n]);
}

/* One candidate change: slot j becomes (act, g); with `br` set, every slot i
 * in (j, k) with br[i] becomes HOLD as well (the bridge, see greedy()). */
typedef struct {
    uint16_t    j, k;
    uint8_t     act;
    float       g;
    const bool *br;           /* NULL: no bridge                           */
} move_t;

/* Saving (base cost - candidate cost) of a move.
 *
 * Only slots from j on can differ, and -- past the bridge -- only until the
 * candidate's battery energy meets the base trajectory again, typically when
 * both run empty or both fill up. From there every slot is identical, so the
 * loop stops. The saving is accumulated slot by slot rather than as a
 * difference of two totals, so float rounding of a 2000 ct horizon cannot
 * fake a 0.01 ct gain. *sj receives the candidate's slot j, *held the AC
 * energy the bridge keeps in the battery that the base discharged. */
static float delta(const ctx_t *c, const em_plan_in_t *in, const em_slot_plan_t *s,
                   uint16_t n, const float *e, const move_t *m, step_t *sj, float *held)
{
    float d = 0.0f;
    float x = slot_step(c, in, m->j, e[m->j], m->act, m->g, sj);
    uint16_t bend = m->br ? m->k : (uint16_t)(m->j + 1);
    d += s[m->j].cost_ct - sj->cost;
    *held = 0.0f;
    for (uint16_t i = m->j + 1; i < n; i++) {
        uint8_t act = s[i].act;
        if (i < bend) {
            if (m->br[i]) {
                act = EM_ACT_HOLD;
                /* NORMAL with a deficit: what the base took from the battery
                 * is the deficit minus what it imported. */
                float def = in->load_kw[i] - in->pv_kw[i] * c->pv_scale;
                if (def > s[i].grid_kw) *held += (def - s[i].grid_kw) * DT_H;
            }
        } else if (fabsf(x - e[i]) < SAME_E_KWH) {
            return d;
        }
        step_t st;
        x = slot_step(c, in, i, x, act, s[i].charge_kw, &st);
        d += s[i].cost_ct - st.cost;
    }
    if (fabsf(x - e[n]) < SAME_E_KWH) return d;
    return d + term_value(c, x) - term_value(c, e[n]);
}

/* The greedy. Returns the number of passes.
 *
 * A pass takes the most expensive slot k that still imports and asks: which
 * single change at an earlier, cheaper slot j saves the most per kWh moved?
 *   HOLD at j   -- j discharges now; keep that energy for later. No
 *                  conversion beyond what the base already pays, so where it
 *                  achieves the same as a grid charge it wins on cost.
 *   CHARGE at j -- one more 0.25 kWh quantum of grid charge.
 * Every candidate is judged by re-simulating the horizon, not by the price
 * pair (j, k): PV that a grid charge displaces is exported at feedin, which
 * makes "charging what the sun would fill anyway" a loss that is simply never
 * accepted.
 *
 * The bridge. Energy added at j is spent at the FIRST slot after j where the
 * battery would otherwise run dry -- with a cheap night between an 18 ct
 * charge and a 45 ct evening, that is the next 20 ct night slot, and the move
 * looks worthless although the plan "charge at 18, keep it, spend it at 45"
 * is the best there is. So each candidate is also tried with HOLD on every
 * slot in (j, k) where the base battery is empty and that is cheaper than k
 * by the spread. Those holds cost (almost) nothing in the base -- there was
 * nothing left to discharge; they only keep the new energy on its way to k.
 * Should one of them turn into an obstacle later (the battery is no longer
 * empty there), release() takes it back.
 *
 * Score: net saving (saving - min_spread * kWh moved) per kWh moved, where
 * "moved" counts everything the move shifts -- the grid charge, the discharge
 * a HOLD or a CHARGE prevents at j, and what the bridge keeps. Counting only
 * the grid charge would let a CHARGE on a discharging slot pass off the
 * implicit HOLD's saving as its own and beat the plain HOLD it contains.
 *
 * The end of the horizon is a slot too (k == n, for_slot EM_FOR_END): the
 * energy left there is worth end_value per kWh, so keeping a kWh out of a
 * cheap slot, or buying one at a very cheap hour, can pay even when nothing
 * inside the horizon imports. This is what "prepare beyond the horizon"
 * means; without it the terminal value could only veto moves, never cause
 * one. It competes like any slot: it is visited in order of its value, so
 * the real expensive slots inside the horizon are served first.
 *
 * A slot k for which nothing pays is set aside; when a round over all
 * importing slots ends with moves accepted, the set-aside slots get another
 * chance, because a later move can open a path to them. */
static void greedy(const ctx_t *c, const em_plan_in_t *in, em_slot_plan_t *s,
                   uint16_t n, float *e, uint16_t *it)
{
    bool     done[EM_MAX_SLOTS + 1];    /* [n] is the end of the horizon    */
    bool     br[EM_MAX_SLOTS];
    uint16_t nbr[EM_MAX_SLOTS + 1];     /* nbr[i] = bridge slots in [0, i)  */
    bool     progress = false;
    memset(done, 0, sizeof(done));

    while (*it < EM_PLAN_MAX_ITER) {
        int   k  = -1;
        float pk = 0.0f;
        /* Only NORMAL slots: a HOLD or CHARGE slot imports because the
         * plan made it, and no move at an earlier slot can change that. */
        for (uint16_t i = 0; i < n; i++) {
            if (done[i] || s[i].act != EM_ACT_NORMAL || s[i].grid_kw <= IMPORT_EPS_KW) continue;
            if (k < 0 || in->price_ct[i] > pk) { k = i; pk = in->price_ct[i]; }
        }
        /* The end "imports" while the battery ends below full. Its value is
         * per DC kWh with losses and wear already taken off -- a fair, if
         * slightly low, stand-in for an AC price. */
        float tv = term_slope(c, e[n]);
        if (!done[n] && tv > 0.0f && e[n] < c->e_full - MIN_MOVE_KWH &&
            (k < 0 || tv > pk)) {
            k = n; pk = tv;
        }
        if (k < 0) {
            if (!progress) break;
            memset(done, 0, sizeof(done));
            progress = false;
            continue;
        }
        (*it)++;

        float lim = pk - c->spread;     /* j and the bridge must be below   */
        nbr[0] = 0;
        for (uint16_t i = 0; i < (uint16_t)k; i++) {
            /* Empty in the base: NORMAL, importing, and not because the
             * discharge power is at its limit. Testing "e at the floor"
             * instead misses the battery that sits a few Wh above it, too
             * little to run the inverter on. */
            float def = in->load_kw[i] - in->pv_kw[i] * c->pv_scale;
            br[i] = s[i].act == EM_ACT_NORMAL && in->price_ct[i] < lim &&
                    s[i].grid_kw > IMPORT_EPS_KW &&
                    def - s[i].grid_kw < c->dis_max - 1e-3f;
            nbr[i + 1] = (uint16_t)(nbr[i] + br[i]);
        }

        float  best = 0.0f;             /* best net saving per kWh moved    */
        move_t bm   = { 0, 0, EM_ACT_NORMAL, 0.0f, NULL };
        bool   found = false;

        for (uint16_t j = 0; j < (uint16_t)k; j++) {
            float pj = in->price_ct[j];
            if (!(pj < lim)) continue;

            step_t b;
            (void)slot_step(c, in, j, e[j], s[j].act, s[j].charge_kw, &b);
            float kept = s[j].act == EM_ACT_NORMAL ? b.p_dis * DT_H : 0.0f;
            bool  can_bridge = nbr[k] - nbr[j + 1] > 0;

            move_t cand[6];
            int    nc = 0;
            if (kept > MIN_MOVE_KWH) {
                cand[nc++] = (move_t){ j, (uint16_t)k, EM_ACT_HOLD, 0.0f, NULL };
                if (can_bridge)
                    cand[nc++] = (move_t){ j, (uint16_t)k, EM_ACT_HOLD, 0.0f, br };
            }
            /* Even at the model's best round trip, a kWh bought at pj comes
             * back worth pk * rt_max minus wear (the end's value has both
             * taken off already). If that is not more than the spread above
             * pj, no simulation is needed to say no. */
            float worth = k == n ? pk : pk * c->rt_max - c->cycle;
            if (c->chg_max > 0.0f && e[j] < c->e_gmax - MIN_MOVE_KWH &&
                pj + c->spread < worth) {
                float g0 = s[j].act == EM_ACT_CHARGE ? s[j].charge_kw : 0.0f;
                if (g0 < c->chg_max - 1e-4f) {
                    float g = g0 + CHG_Q_KW;
                    if (g > c->chg_max) g = c->chg_max;
                    cand[nc++] = (move_t){ j, (uint16_t)k, EM_ACT_CHARGE, g, NULL };
                    if (can_bridge)
                        cand[nc++] = (move_t){ j, (uint16_t)k, EM_ACT_CHARGE, g, br };
                    /* The whole slot at once. The inverter's own draw while
                     * charging (~0.1 kW) eats half of a first 0.25 kW step,
                     * so on a slot not yet charging that step never pays --
                     * and the plan bought later at a dearer price what a
                     * full cheap slot would have covered (operator,
                     * 2026-10-25). release() trims what is too much. */
                    if (g < c->chg_max - 1e-4f) {
                        cand[nc++] = (move_t){ j, (uint16_t)k, EM_ACT_CHARGE, c->chg_max, NULL };
                        if (can_bridge)
                            cand[nc++] = (move_t){ j, (uint16_t)k, EM_ACT_CHARGE, c->chg_max, br };
                    }
                }
            }

            for (int q = 0; q < nc; q++) {
                step_t cs;
                float  held;
                float  d = delta(c, in, s, n, e, &cand[q], &cs, &held);
                float  moved = kept + held;
                if (cand[q].act == EM_ACT_CHARGE) {
                    float gm = (cs.g_used - b.g_used) * DT_H;
                    if (gm <= MIN_MOVE_KWH) continue;   /* soc_max: nothing to add */
                    moved += gm;
                }
                float net = d - c->spread * moved;
                if (net > MIN_GAIN_CT && net / moved > best) {
                    best = net / moved; bm = cand[q]; found = true;
                }
            }
        }

        if (!found) { done[k] = true; continue; }

        /* The first (= most expensive) slot a move was made for is the
         * explanation; later quanta on the same slot keep it. */
        s[bm.j].act       = bm.act;
        s[bm.j].charge_kw = bm.act == EM_ACT_CHARGE ? bm.g : 0.0f;
        int16_t why = k == n ? EM_FOR_END : (int16_t)k;
        if (s[bm.j].for_slot == -1) s[bm.j].for_slot = why;
        if (bm.br) {
            for (uint16_t i = bm.j + 1; i < bm.k; i++) {
                if (!bm.br[i]) continue;
                s[i].act = EM_ACT_HOLD;
                if (s[i].for_slot == -1) s[i].for_slot = why;
            }
        }
        (void)run(c, in, s, n, e);
        progress = true;
    }
}

/* Take back what no longer pays. Returns true when anything changed.
 *
 * The greedy only ever adds. A move that was right when it was made can be
 * dead weight a few moves later: a bridge that held a 33 ct evening slot
 * while the battery was empty there blocks that slot once a night charge has
 * filled the battery; a hold made before a charge may keep energy nobody
 * needs any more. So every HOLD is tried as NORMAL and every CHARGE one
 * quantum lower (the last quantum makes it a HOLD, which the next round may
 * release in turn). A release is taken when the plan gets no dearer: a hold
 * that changes nothing goes too, because a plan full of no-op holds explains
 * nothing. Best first, one at a time, each judged on the current plan. */
static bool release(const ctx_t *c, const em_plan_in_t *in, em_slot_plan_t *s,
                    uint16_t n, float *e, uint16_t *it)
{
    bool any = false;
    while (*it < EM_PLAN_MAX_ITER) {
        float   best = -MIN_GAIN_CT * 0.1f;   /* "no dearer", float-tolerant */
        int     bj   = -1;
        uint8_t bact = EM_ACT_NORMAL;
        float   bg   = 0.0f;
        for (uint16_t j = 0; j < n; j++) {
            if (s[j].act == EM_ACT_NORMAL) continue;
            move_t m = { j, j, EM_ACT_NORMAL, 0.0f, NULL };
            if (s[j].act == EM_ACT_CHARGE) {
                m.g   = s[j].charge_kw - CHG_Q_KW;
                m.act = m.g > 1e-4f ? EM_ACT_CHARGE : EM_ACT_HOLD;
                if (m.act == EM_ACT_HOLD) m.g = 0.0f;
            }
            step_t cs;
            float  held;
            float  d = delta(c, in, s, n, e, &m, &cs, &held);
            if (d >= best) { best = d; bj = j; bact = m.act; bg = m.g; }
        }
        if (bj < 0) break;
        (*it)++;
        s[bj].act       = bact;
        s[bj].charge_kw = bg;
        if (bact == EM_ACT_NORMAL) s[bj].for_slot = -1;
        (void)run(c, in, s, n, e);
        any = true;
    }
    return any;
}

/* Median and lower quartile of the known prices. */
static void price_stats(const em_plan_in_t *in, uint16_t n, float *med, float *p25)
{
    float a[EM_MAX_SLOTS];
    for (uint16_t i = 0; i < n; i++) {        /* insertion sort, n <= 192 */
        float v = in->price_ct[i];
        int   j = (int)i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
    *med = (n & 1u) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
    float q = 0.25f * (float)(n - 1);         /* linear interpolation */
    uint16_t k = (uint16_t)q;
    *p25 = k + 1 < n ? a[k] + (q - (float)k) * (a[k + 1] - a[k]) : a[k];
}

static void ctx_init(ctx_t *c, const em_plan_in_t *in, uint16_t n)
{
    memset(c, 0, sizeof(*c));

    const float *ld = in->loss_dis, *lc = in->loss_chg;
    if (ld[0] == 0.0f && ld[1] == 0.0f && ld[2] == 0.0f) {
        c->ld[0] = EM_LOSS_DIS_A; c->ld[1] = EM_LOSS_DIS_B; c->ld[2] = EM_LOSS_DIS_C;
    } else {
        for (int i = 0; i < 3; i++) c->ld[i] = sane(ld[i], 0.0f);
    }
    if (lc[0] == 0.0f && lc[1] == 0.0f && lc[2] == 0.0f) {
        c->lc[0] = EM_LOSS_CHG_A; c->lc[1] = EM_LOSS_CHG_B; c->lc[2] = EM_LOSS_CHG_C;
    } else {
        for (int i = 0; i < 3; i++) c->lc[i] = sane(lc[i], 0.0f);
    }

    float conf = in->pv_confidence;
    c->pv_scale = (isfinite(conf) && conf > 0.0f) ? (conf > 1.0f ? 1.0f : conf) : 1.0f;
    c->spread   = sane(in->min_spread_ct, 0.0f);
    c->cycle    = sane(in->cycle_cost_ct, 0.0f);
    c->feedin   = isfinite(in->feedin_ct) ? in->feedin_ct : 0.0f;
    c->dis_max  = sane(in->dis_kw_max, 0.0f);
    c->chg_max  = sane(in->charge_kw, 0.0f);

    float cap = sane(in->cap_kwh, 0.0f);
    c->battery = cap > 0.0f && isfinite(in->soc_pct);
    if (!c->battery) {
        /* No battery to speak of: zero-sized, so the same simulator yields
         * plain PV + load and every move finds nothing to move. */
        c->dis_max = c->chg_max = 0.0f;
        return;
    }

    float smin = sane(in->soc_min_pct, 0.0f);
    if (smin > 100.0f) smin = 100.0f;
    float smax = isfinite(in->soc_max_pct) ? in->soc_max_pct : 100.0f;
    if (smax > 100.0f) smax = 100.0f;
    if (smax < smin)   smax = smin;
    float soc = in->soc_pct < 0.0f ? 0.0f : (in->soc_pct > 100.0f ? 100.0f : in->soc_pct);

    c->e_full = cap;
    c->e_min  = cap * smin / 100.0f;
    c->e_gmax = cap * smax / 100.0f;
    c->e0     = cap * soc / 100.0f;

    /* Value of a stored DC kWh after the horizon: delivered at 1 kW (a
     * typical evening load) at the horizon's median price, minus the wear it
     * will cost to get it out. Not a forecast of tomorrow, just "energy in
     * the battery is worth something" -- enough to stop the plan from
     * dumping it in the last slots or from never preparing for the next day.
     * Wear is subtracted so that keeping a kWh and spending it are judged
     * alike; without it, ending full would look cheaper than discharging. */
    float eta_d1 = 1.0f / (1.0f + loss_kw(c->ld, 1.0f));
    float pmed, p25;
    price_stats(in, n, &pmed, &p25);
    float v = eta_d1 * pmed - c->cycle;
    c->term_per_kwh = v > 0.0f ? v : 0.0f;

    /* With the forecast reaching past the prices: the end value is what the
     * energy SAVES afterwards. Run the battery on from each end level through
     * the known PV and load (NORMAL, no grid charge), buy what is missing at
     * the median price, and keep the flat value only for what is still in
     * the battery when the forecast ends. A sunny next day refills the
     * battery by noon: energy carried over is then worth the night's
     * deficit, not its size -- so the plan neither hoards in the evening nor
     * buys for a day the sun will cover (operator, 2026-10-22: "60 kWh today
     * should last two days"). Unknown prices after the horizon are taken at
     * the median of the known ones. What is still stored when even the
     * forecast ends is valued at the LOWER QUARTILE: that far out it most
     * likely replaces a cheap kWh, not an average one -- at the median, a
     * flat price day made holding a full battery through the night look
     * worth it (operator, 2026-10-25: "why hold tonight, there is enough"). */
    uint16_t ext = in->n_ext;
    if (ext > EM_MAX_SLOTS - n) ext = (uint16_t)(EM_MAX_SLOTS - n);
    for (uint16_t i = 0; i < ext; i++)
        if (!isfinite(in->pv_kw[n + i]) || !isfinite(in->load_kw[n + i])) { ext = i; break; }
    if (ext >= 4 && c->e_full > c->e_min + 1e-3f) {
        float v_res = eta_d1 * p25 - c->cycle;
        if (v_res < 0.0f) v_res = 0.0f;
        c->term_step = (c->e_full - c->e_min) / (float)(TERM_N - 1);
        for (int t = 0; t < TERM_N; t++) {
            float e0 = c->e_min + (float)t * c->term_step, e = e0, cost = 0.0f;
            for (uint16_t i = 0; i < ext; i++) {
                step_t st;
                float ne = slot_step(c, in, (uint16_t)(n + i), e, EM_ACT_NORMAL, 0.0f, &st);
                float g  = st.grid;
                cost += (g > 0.0f ? g * pmed : g * c->feedin) * DT_H;
                if (ne < e) cost += c->cycle * (e - ne);
                e = ne;
            }
            float left = e - c->e_min;
            c->term_tab[t] = -cost + (left > 0.0f ? left * v_res : 0.0f);
        }
        /* Relative to an empty battery, and never falling: more energy can
         * always be left unused (float noise aside). */
        float base = c->term_tab[0];
        for (int t = 0; t < TERM_N; t++) {
            c->term_tab[t] -= base;
            if (t && c->term_tab[t] < c->term_tab[t - 1]) c->term_tab[t] = c->term_tab[t - 1];
        }
        c->term_tab_ok = true;
    }

    /* Best round trip the loss model allows, for pruning charge candidates.
     * Scanned rather than derived, so any coefficients work. */
    float bc = 0.0f, bd = 0.0f;
    for (float p = 0.25f; p <= 20.0f; p += 0.25f) {
        float ec = (p - loss_kw(c->lc, p)) / p;
        float ed = p / (p + loss_kw(c->ld, p));
        if (ec > bc) bc = ec;
        if (ed > bd) bd = ed;
    }
    c->rt_max = bc * bd;
}

void em_plan_compute(const em_plan_in_t *in, em_plan_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < EM_MAX_SLOTS; i++) out->s[i].for_slot = -1;
    if (!in) return;
    out->t0 = in->t0;

    /* The horizon: up to the first slot anything is unknown. A plan across a
     * hole would have to invent the missing numbers. */
    uint16_t n = in->n < EM_MAX_SLOTS ? in->n : EM_MAX_SLOTS;
    for (uint16_t i = 0; i < n; i++) {
        if (!isfinite(in->price_ct[i]) || !isfinite(in->pv_kw[i]) ||
            !isfinite(in->load_kw[i])) {
            n = i;
            break;
        }
    }
    out->n = n;
    if (n == 0) return;

    ctx_t c;
    ctx_init(&c, in, n);

    float e[EM_MAX_SLOTS + 1];
    out->cost_base_ct = run(&c, in, out->s, n, e);   /* all NORMAL */

    if (c.battery) {
        /* Add, then take back what no longer pays, then look again: freeing
         * a slot can open a better move. Each round strictly improves (the
         * greedy) or keeps (release) the cost; the shared pass budget bounds
         * the whole thing. */
        uint16_t it = 0;
        for (int round = 0; round < 4; round++) {
            greedy(&c, in, out->s, n, e, &it);
            if (!release(&c, in, out->s, n, e, &it)) break;
        }
        out->iterations = it;
    }

    out->cost_plan_ct = run(&c, in, out->s, n, e);
    out->end_value_ct = term_value(&c, e[n]);
    for (uint16_t i = 0; i < n; i++)
        out->s[i].soc_end = c.battery ? e[i + 1] * 100.0f / c.e_full : 0.0f;
}

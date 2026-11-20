#include "bat_cost.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include "nvs_store.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bat_cost";

#define MAX_STEP_S      120     /* a longer gap is skipped, not filled        */
#define GRID_MAX_AGE_MS 15000
#define DEF_GRID_CT     30.0f   /* only until the feed has delivered once     */
#define DEF_ETA         0.95f   /* Deye, until its long-term measurement has  */
#define MIN_LT_WH       5000.0  /* ... this much energy behind it             */

/* NVS record. Fields are only ever appended. */
typedef struct {
    bc_pack_t  deye, byd;
    bc_stats_t sd, sb;
    int64_t    since;
    float      grid_ct;         /* last grid price from the feed              */
    uint8_t    have_deye, have_byd;
    uint8_t    have_grid;       /* grid_ct is set (0 ct is a real price)      */
    uint8_t    _rsv;
} rec_t;

static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static rec_t             s_rec, s_saved;
static bat_cost_status_t s_view;
static volatile bool     s_reset_req;
static int64_t           s_last_us;
static uint32_t          s_saved_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void save(bool force)
{
    rec_t r;
    portENTER_CRITICAL(&s_mux);
    r = s_rec;
    portEXIT_CRITICAL(&s_mux);
    if (memcmp(&r, &s_saved, sizeof(r)) == 0) return;
    uint32_t t = now_ms();
    if (!force && (uint32_t)(t - s_saved_ms) < BAT_COST_SAVE_MS) return;
    if (nvs_store_set_bat_cost(&r, sizeof(r)) == ESP_OK) {
        s_saved    = r;
        s_saved_ms = t;
    }
}

void bat_cost_save_now(void) { save(true); }

void bat_cost_init(void)
{
    rec_t r;
    memset(&r, 0, sizeof(r));
    if (nvs_store_get_bat_cost(&r, sizeof(r)) == ESP_OK &&
        isfinite(r.deye.ct) && isfinite(r.byd.ct) && r.deye.ct >= 0 && r.byd.ct >= 0) {
        portENTER_CRITICAL(&s_mux);
        s_rec = r;
        portEXIT_CRITICAL(&s_mux);
        s_saved = r;
        ESP_LOGI(TAG, "loaded: Deye %.2f ct/kWh (cells, %.0f %% grid), BYD %.2f ct/kWh",
                 r.deye.ct, r.deye.grid * 100, r.byd.ct);
    }
    s_view.deye.content_kwh = s_view.byd.content_kwh = NAN;
    /* No shutdown handler of its own: IDF has room for five, and they are
     * taken (esp_timer, WiFi, modbus_rtu, byd_energy, energy_mgr). The
     * energy manager's handler calls bat_cost_save_now(). */
}

void bat_cost_reset(void) { s_reset_req = true; }

/* The Deye's efficiencies from its long-term conversion record (Wh, DC and
 * AC side of every charge and discharge since commissioning of the record). */
static void deye_eta(const mb_deye_live_t *l, float *ec, float *ed, bool *measured)
{
    const mb_deye_eff_lt_t *lt = &l->lt;
    *ec = *ed = DEF_ETA;
    *measured = false;
    if (lt->chg_ac > MIN_LT_WH && lt->dis_dc > MIN_LT_WH) {
        double c = lt->chg_dc / lt->chg_ac, d = lt->dis_ac / lt->dis_dc;
        if (c > 0.7 && c <= 1.0 && d > 0.7 && d <= 1.0) {
            *ec = (float)c; *ed = (float)d; *measured = true;
        }
    }
}

static void fill(bat_cost_bat_t *v, bool have, const bc_pack_t *p, const bc_stats_t *st,
                 float content, float ec, float ed)
{
    v->valid       = have;
    v->eta_c       = ec;
    v->eta_d       = ed;
    v->content_kwh = content;
    v->st          = *st;
    v->out_ct      = have ? (float)bc_out_ct(p, ed) : NAN;
    v->grid_pct    = have ? (float)(p->grid * 100.0) : NAN;
    v->value_eur   = (have && isfinite(content)) ? (float)(content * p->ct / 100.0) : NAN;
}

void bat_cost_tick(int64_t now, const em_cfg_t *c, float price_now_ct)
{
    int64_t us = esp_timer_get_time();
    double dt_h = s_last_us ? (us - s_last_us) / 3.6e9 : 0;
    s_last_us = us;

    /* ---- what the batteries are doing ---- */
    static mb_deye_live_t l;                 /* ~0.5 kB: not on the task stack */
    modbus_rtu_get_deye_live(&l);
    float ec_d, ed_d; bool measured;
    deye_eta(&l, &ec_d, &ed_d, &measured);
    bool deye_ok = l.valid && l.online && (l.blocks & MB_DEYE_BLK_BAT);
    bc_bat_t deye = { NAN, NAN, ec_d, ed_d };
    if (deye_ok) {
        deye.dc_w = l.bat_p;                                 /* cell side, + discharge */
        if (c->deye_kwh > 0) deye.content_kwh = l.bat_soc / 100.0f * c->deye_kwh;
    }

    float rt = (c->byd_eff_pct ? c->byd_eff_pct : EM_DEF_BYD_EFF) / 100.0f;
    float e_b = sqrtf(rt);
    modbus_tcp_status_t ts;
    modbus_tcp_get_status(&ts);
    bool byd_ok = ts.dev_count && ts.connected && ts.byd_soc > 0.0f;
    bc_bat_t byd = { NAN, NAN, e_b, e_b };
    if (byd_ok) {
        /* byd_w is the battery's share on the AC side (+ discharge): back to
         * the cells with the assumed efficiency */
        byd.dc_w = ts.byd_w > 0 ? ts.byd_w / e_b : ts.byd_w * e_b;
        if (c->byd_kwh > 0) byd.content_kwh = ts.byd_soc / 100.0f * c->byd_kwh;
    }

    float grid_w = NAN;
    bool grid_ok = modbus_tcp_grid_w_fresh(&grid_w, GRID_MAX_AGE_MS);

    bool active = c->feedin_ct > 0.0f;
    portENTER_CRITICAL(&s_mux);
    rec_t r = s_rec;
    portEXIT_CRITICAL(&s_mux);

    if (s_reset_req) {
        s_reset_req = false;
        float g = r.grid_ct;
        uint8_t hg = r.have_grid;
        memset(&r, 0, sizeof(r));
        r.grid_ct = g; r.have_grid = hg;
        ESP_LOGI(TAG, "reset: both batteries start again at the feed-in tariff");
    }
    bool live = isfinite(price_now_ct) && price_now_ct > -100.0f;
    if (live) { r.grid_ct = price_now_ct; r.have_grid = 1; }
    float grid_ct = r.have_grid ? r.grid_ct : DEF_GRID_CT;

    if (active) {
        /* Start value: whatever is in a battery when it is first seen counts
         * as solar (the operator's choice). */
        if (!r.have_deye && bc_bat_known(&deye)) { bc_pack_init(&r.deye, c->feedin_ct, deye.eta_c); r.have_deye = 1; }
        if (!r.have_byd  && bc_bat_known(&byd))  { bc_pack_init(&r.byd,  c->feedin_ct, byd.eta_c);  r.have_byd  = 1; }
        if ((r.have_deye || r.have_byd) && !r.since) r.since = now;

        if (grid_ok && dt_h > 0 && dt_h * 3600 <= MAX_STEP_S) {
            /* a battery without a pot yet is no source either */
            bc_bat_t d = deye, b = byd;
            if (!r.have_deye) d.content_kwh = d.dc_w = NAN;
            if (!r.have_byd)  b.content_kwh = b.dc_w = NAN;
            bc_step(&r.deye, &r.byd, &d, &b, grid_w, grid_ct, c->feedin_ct, dt_h, &r.sd, &r.sb);
        }
    }

    portENTER_CRITICAL(&s_mux);
    s_rec = r;
    s_view.active    = active;
    s_view.feedin_ct = c->feedin_ct;
    s_view.grid_ct   = grid_ct;
    s_view.grid_live = live;
    s_view.since     = r.since;
    fill(&s_view.deye, active && r.have_deye, &r.deye, &r.sd, deye.content_kwh, ec_d, ed_d);
    fill(&s_view.byd,  active && r.have_byd,  &r.byd,  &r.sb, byd.content_kwh,  e_b,  e_b);
    s_view.deye.eta_measured = measured;
    portEXIT_CRITICAL(&s_mux);

    save(false);
}

void bat_cost_get(bat_cost_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_view;
    portEXIT_CRITICAL(&s_mux);
}

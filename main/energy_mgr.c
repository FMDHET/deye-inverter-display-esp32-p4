#include "energy_mgr.h"
#include "em_cfg.h"
#include "price_feed.h"
#include "pv_forecast.h"
#include "load_profile.h"
#include "fronius_storage.h"
#include "deye_ctrl.h"
#include "eff_sweep.h"
#include "modbus_rtu.h"
#include "bat_lock.h"
#include "deye_force.h"
#include "em_hist.h"
#include "modbus_tcp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "energy_mgr";

#define TICK_MS          10000      /* feeds and actions                      */
#define HOLD_RENEW_S     120        /* hold expires this long after the last tick */
#define BYD_REVERT_S     1800       /* Fronius falls back on its own after this */
#define CLOCK_OK         1700000000LL

/* Big state lives here, not on the task stack. The plan is computed into
 * s_work and swapped into s_plan under the lock, so readers never see half a
 * plan. */
static EXT_RAM_BSS_ATTR em_plan_in_t s_in;
static EXT_RAM_BSS_ATTR em_plan_t    s_work, s_plan;
static em_status_t       s_st;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static volatile uint8_t  s_req;            /* REQ_* bits from the web/MQTT */
#define REQ_PRICES   0x01
#define REQ_FORECAST 0x02
#define REQ_PLAN     0x04

/* What we put into the Deye ourselves, so an outside change can be told
 * apart (the same idea as eff_sweep). */
static bool     s_own_charge;              /* our forced charge is in force   */
static int      s_own_power;
static int64_t  s_block_end;               /* end of the block we serve       */
static bool     s_own_hold, s_own_byd;
static int64_t  s_paused_until;            /* user took over until then       */

static int64_t  s_last_plan_slot = -1;
static volatile int64_t s_fronius_wanted;  /* the web page shows the Fronius card */
static int64_t  s_price_seen, s_pv_seen;   /* fetched stamps the plan used    */

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* "14,2" -- the page and MQTT are German. */
static void ct_str(char *b, size_t n, float v)
{
    snprintf(b, n, "%.1f", v);
    for (char *p = b; *p; p++) if (*p == '.') *p = ',';
}

static void hhmm(char *b, size_t n, int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(b, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* ------------------------------ planning ------------------------------- */

/* Gather the inputs for a plan starting at the current quarter hour. The
 * horizon is as long as the prices reach; the planner stops at the first NAN
 * of price, PV or load on its own. */
static bool build_inputs(int64_t now, const em_cfg_t *c, char *err, size_t errlen)
{
    static EXT_RAM_BSS_ATTR price_set_t ps;
    static EXT_RAM_BSS_ATTR pv_fc_t     pv;
    static EXT_RAM_BSS_ATTR em_series_t load;
    price_feed_get(&ps);
    pv_forecast_get(&pv);

    memset(&s_in, 0, sizeof(s_in));
    s_in.t0 = em_slot_floor(now);
    if (ps.total.n == 0 || isnan(em_series_at(&ps.total, s_in.t0))) {
        snprintf(err, errlen, "keine Strompreise%s%.60s", ps.err[0] ? ": " : "", ps.err);
        return false;
    }
    load_profile_forecast(s_in.t0, EM_MAX_SLOTS, &load);

    /* The load profile learns for two weeks. Until a slot is known, use the
     * mean of what is known, or 0.6 kW for a house that has not been watched
     * at all -- a plan with a rough load beats no plan. */
    float sum = 0; int cnt = 0;
    for (int i = 0; i < load.n; i++) if (!isnan(load.v[i])) { sum += load.v[i]; cnt++; }
    float load_fb = cnt ? sum / cnt : 0.6f;

    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    if (!(l.valid && l.online)) { snprintf(err, errlen, "Deye antwortet nicht"); return false; }

    uint16_t n = 0;
    for (; n < EM_MAX_SLOTS; n++) {
        int64_t t = s_in.t0 + (int64_t)n * EM_SLOT_S;
        float p = em_series_at(&ps.total, t);
        if (isnan(p)) break;
        float v = em_series_at(&pv.total_kw, t);
        float ld = em_series_at(&load, t);
        s_in.price_ct[n] = p;
        s_in.pv_kw[n]    = isnan(v) ? 0.0f : v;      /* no forecast: plan without PV */
        s_in.load_kw[n]  = isnan(ld) ? load_fb : ld;
    }
    s_in.n             = n;
    /* After the last price: the rest of the PV forecast (up to tomorrow
     * 24:00) with the load profile, so the planner values energy left at the
     * end by what it saves -- not flat. No forecast there: n_ext stays 0. */
    uint16_t m = n;
    for (; m < EM_MAX_SLOTS; m++) {
        int64_t t = s_in.t0 + (int64_t)m * EM_SLOT_S;
        float v = em_series_at(&pv.total_kw, t);
        if (isnan(v)) break;
        float ld = em_series_at(&load, t);
        s_in.price_ct[m] = NAN;
        s_in.pv_kw[m]    = v;
        s_in.load_kw[m]  = isnan(ld) ? load_fb : ld;
    }
    /* Past the forecast as well, to the end of the table: the same quarter
     * hour one day earlier ("tomorrow like today"). Once the prices reach as
     * far as the forecast, nothing followed the horizon and the energy left
     * at the end was worth the flat median -- on a flat price day that made
     * holding a full battery through the night look right (operator,
     * 2026-10-25). A guess, but a day with PV in it: energy carried over is
     * worth what it saves until the sun comes, not a whole battery. */
    for (; m < EM_MAX_SLOTS; m++) {
        int64_t t = s_in.t0 + (int64_t)m * EM_SLOT_S;
        float v = em_series_at(&pv.total_kw, t - 86400);
        if (isnan(v) && m >= 96) v = s_in.pv_kw[m - 96];
        if (isnan(v)) break;
        float ld = em_series_at(&load, t);
        s_in.price_ct[m] = NAN;
        s_in.pv_kw[m]    = v;
        s_in.load_kw[m]  = isnan(ld) ? load_fb : ld;
    }
    s_in.n_ext         = (uint16_t)(m - n);
    s_in.soc_pct       = l.bat_soc;
    s_in.cap_kwh       = c->deye_kwh;
    s_in.soc_min_pct   = c->soc_min_pct;
    s_in.soc_max_pct   = c->soc_max_pct;
    s_in.charge_kw     = c->charge_kw;
    s_in.dis_kw_max    = 10.0f;
    s_in.min_spread_ct = c->min_spread_ct;
    s_in.cycle_cost_ct = c->cycle_cost_ct;
    s_in.feedin_ct     = 0.0f;
    s_in.pv_confidence = 0.8f;
    if (c->deye_kwh <= 0.0f) { snprintf(err, errlen, "Speichergröße des Deye fehlt (Einstellungen)"); return false; }
    if (n < 4) { snprintf(err, errlen, "zu wenige Preise für einen Plan"); return false; }
    s_price_seen = ps.fetched;
    s_pv_seen    = pv.fetched;
    return true;
}

static void plan_now(int64_t now, const em_cfg_t *c)
{
    char err[96] = "";
    if (!build_inputs(now, c, err, sizeof(err))) {
        lock();
        snprintf(s_st.err, sizeof(s_st.err), "%s", err);
        s_plan.n = 0;
        unlock();
        return;
    }
    int64_t t0 = esp_timer_get_time();
    em_plan_compute(&s_in, &s_work);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    lock();
    s_plan = s_work;
    s_st.updated = now;
    s_st.err[0] = '\0';
    unlock();
    ESP_LOGI(TAG, "plan: %u slots (+%u forecast only), %u iterations, %d ms, %.0f -> %.0f ct, end value %.0f ct",
             s_work.n, s_in.n_ext, s_work.iterations, ms, s_work.cost_base_ct, s_work.cost_plan_ct,
             s_work.end_value_ct);
}

/* ------------------------------ acting --------------------------------- */

/* End of the block of identical actions starting at slot i. */
static int block_end(const em_plan_t *p, int i)
{
    int j = i;
    while (j + 1 < p->n && p->s[j + 1].act == p->s[i].act) j++;
    return j;
}

static void release(const char *why)
{
    if (s_own_charge && deye_ctrl_get_mode() == DEYE_MODE_FORCE_CHARGE &&
        deye_ctrl_get_user_power() == s_own_power) {
        deye_ctrl_apply(DEYE_MODE_NORMAL, deye_ctrl_get_user_power());
        ESP_LOGI(TAG, "charge released (%s)", why);
    }
    s_own_charge = false;
    if (s_own_hold) { modbus_rtu_set_hold(0); s_own_hold = false; }
    if (s_own_byd)  { fronius_storage_block_discharge(false, 0); s_own_byd = false; }
}

static void act(int64_t now, const em_cfg_t *c)
{
    lock();
    int i = (s_plan.n && now >= s_plan.t0) ? (int)((now - s_plan.t0) / EM_SLOT_S) : -1;
    if (i >= s_plan.n) i = -1;
    em_slot_plan_t sp = i >= 0 ? s_plan.s[i] : (em_slot_plan_t){ .act = EM_ACT_NORMAL, .for_slot = -1 };
    int be = i >= 0 ? block_end(&s_plan, i) : -1;
    int64_t bend = be >= 0 ? s_plan.t0 + (int64_t)(be + 1) * EM_SLOT_S : now;
    float target = sp.soc_end;
    if (i >= 0) for (int k = i; k <= be; k++) if (s_plan.s[k].soc_end > target) target = s_plan.s[k].soc_end;
    unlock();

    bool auto_on = c->mode == EM_MODE_AUTO && !eff_sweep_running();

    /* The user took over: our charge is no longer what deye_ctrl holds --
     * or a forced mode from the web page runs (same power as ours or not). */
    deye_force_t df;
    deye_force_get(&df);
    if (s_own_charge && (df.mode || deye_ctrl_get_mode() != DEYE_MODE_FORCE_CHARGE ||
                         deye_ctrl_get_user_power() != s_own_power)) {
        s_own_charge   = false;
        s_paused_until = s_block_end > now ? s_block_end : now + 3600;
        ESP_LOGW(TAG, "manual change -- automation paused until the block ends");
    }
    bool paused = now < s_paused_until;
    if (!auto_on || paused || i < 0) {
        release(!auto_on ? "not in AUTO" : (paused ? "paused" : "no plan"));
        lock(); s_st.acting = false; s_st.paused = paused; unlock();
        return;
    }
    /* A manual forced mode in force (not ours) also means: hands off. */
    if (!s_own_charge && deye_ctrl_get_mode() != DEYE_MODE_NORMAL) {
        release("manual forced mode");
        lock(); s_st.acting = false; s_st.paused = true; unlock();
        return;
    }

    switch (sp.act) {
    case EM_ACT_CHARGE: {
        if (s_own_hold) { modbus_rtu_set_hold(0); s_own_hold = false; }
        int w = (int)(sp.charge_kw * 1000.0f + 0.5f);
        int soc = (int)(target + 0.5f);
        if (soc > c->soc_max_pct) soc = c->soc_max_pct;
        if (!s_own_charge || s_block_end != bend) {
            uint32_t lim = (uint32_t)(bend - now + 600);
            deye_ctrl_apply_auto(DEYE_MODE_FORCE_CHARGE, w, soc, lim);
            s_own_charge = true;
            s_own_power  = deye_ctrl_get_user_power();
            s_block_end  = bend;
            ESP_LOGI(TAG, "charge %d W to %d %% until block end", w, soc);
        }
        break;
    }
    case EM_ACT_HOLD:
        if (s_own_charge) release("hold follows");
        modbus_rtu_set_hold(HOLD_RENEW_S);
        s_own_hold  = true;
        s_block_end = bend;
        break;
    default:
        release("normal");
        break;
    }

    bool want_byd = c->byd_ctrl && sp.act != EM_ACT_NORMAL;
    if (want_byd) { fronius_storage_block_discharge(true, BYD_REVERT_S); s_own_byd = true; }
    else if (s_own_byd) { fronius_storage_block_discharge(false, 0); s_own_byd = false; }

    lock(); s_st.acting = true; s_st.paused = false; unlock();
}

/* --------------------------- status in words --------------------------- */

static void describe(int64_t now)
{
    static EXT_RAM_BSS_ATTR price_set_t ps;
    static EXT_RAM_BSS_ATTR grid_mix_t  mx;
    static EXT_RAM_BSS_ATTR pv_fc_t     pv;
    price_feed_get(&ps);
    price_feed_get_mix(&mx);
    pv_forecast_get(&pv);
    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    float pv_today = 0, pv_tom = 0;
    for (int k = 0; k < pv.n; k++) { pv_today += pv.p[k].kwh_today; pv_tom += pv.p[k].kwh_tomorrow; }

    lock();
    em_status_t *st = &s_st;
    st->price_now_ct  = em_series_at(&ps.total, now);
    st->ren_now_pct   = em_series_at(&mx.ren, now);
    st->solar_now_pct = em_series_at(&mx.solar, now);
    float won = em_series_at(&mx.wind_on, now), woff = em_series_at(&mx.wind_off, now);
    st->wind_now_pct  = (isnan(won) && isnan(woff)) ? NAN : (isnan(won) ? 0 : won) + (isnan(woff) ? 0 : woff);
    st->pv_today_kwh = pv.n ? pv_today : NAN;
    st->pv_tomorrow_kwh = pv.n ? pv_tom : NAN;
    st->deye_soc = (l.valid && l.online) ? l.bat_soc : NAN;

    const em_plan_t *p = &s_plan;
    int i = (p->n && now >= p->t0) ? (int)((now - p->t0) / EM_SLOT_S) : -1;
    if (i >= p->n) i = -1;
    st->act_now   = i >= 0 ? p->s[i].act : EM_ACT_NORMAL;
    st->saving_ct = p->n ? p->cost_base_ct - p->cost_plan_ct : 0.0f;
    st->next_charge_from = st->next_charge_to = 0;
    for (int k = i < 0 ? 0 : i; k < p->n; k++)
        if (p->s[k].act == EM_ACT_CHARGE) {
            int e = block_end(p, k);
            st->next_charge_from = p->t0 + (int64_t)k * EM_SLOT_S;
            st->next_charge_to   = p->t0 + (int64_t)(e + 1) * EM_SLOT_S;
            break;
        }

    char a[16], b[16], t[8];
    if (st->err[0] && !p->n) {
        snprintf(st->why, sizeof(st->why), "kein Plan: %.100s", st->err);
    } else if (i < 0) {
        snprintf(st->why, sizeof(st->why), "kein Plan für jetzt");
    } else {
        const em_slot_plan_t *s = &p->s[i];
        int f = s->for_slot;
        ct_str(a, sizeof(a), p->n ? s_in.price_ct[i] : NAN);
        if (s->act != EM_ACT_NORMAL && f == EM_FOR_END) {
            /* energy for after the known prices (tomorrow is not out yet) */
            snprintf(st->why, sizeof(st->why), "%s: jetzt %s ct, Vorrat für nach dem Planungshorizont",
                     s->act == EM_ACT_HOLD ? "Halten" : "Laden", a);
        } else if (s->act == EM_ACT_NORMAL || f < 0 || f >= p->n) {
            snprintf(st->why, sizeof(st->why), s->act == EM_ACT_NORMAL ? "Normalbetrieb"
                     : (s->act == EM_ACT_HOLD ? "Halten" : "Laden"));
        } else {
            ct_str(b, sizeof(b), s_in.price_ct[f]);
            hhmm(t, sizeof(t), p->t0 + (int64_t)f * EM_SLOT_S);
            if (s->act == EM_ACT_CHARGE) {
                char kw[16]; ct_str(kw, sizeof(kw), s->charge_kw);
                snprintf(st->why, sizeof(st->why), "Laden mit %s kW: jetzt %s ct, gebraucht um %s (%s ct)", kw, a, t, b);
            } else {
                snprintf(st->why, sizeof(st->why), "Halten: jetzt %s ct, Energie für %s (%s ct)", a, t, b);
            }
        }
        if (s_st.paused) strncat(st->why, " – pausiert (Handbedienung)", sizeof(st->why) - strlen(st->why) - 1);
        else if (st->mode == EM_MODE_SHADOW && s->act != EM_ACT_NORMAL)
            strncat(st->why, " – nur geplant", sizeof(st->why) - strlen(st->why) - 1);
    }
    unlock();
}

/* ------------------------------ history -------------------------------- */

/* One sample for the quarter-hour record (em_hist.h). The plan's view is the
 * one describe() left in s_st on the previous tick -- ten seconds late, which
 * a quarter hour does not notice. */
static void hist_tick(int64_t now, const em_cfg_t *c)
{
    eh_sample_t s;
    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    bool deye = l.valid && l.online;
    s.deye_soc   = deye ? l.bat_soc : NAN;
    s.deye_bat_w = deye ? l.bat_p : NAN;
    modbus_tcp_status_t ts;
    modbus_tcp_get_status(&ts);
    bool tcp = ts.dev_count && ts.connected;
    s.byd_soc = tcp && ts.byd_soc > 0.0f ? ts.byd_soc : NAN;
    s.grid_w  = tcp ? ts.grid_w : NAN;
    s.pv_w    = tcp ? ts.pv_w : NAN;
    s.house_w = tcp ? ts.house_w : NAN;
    lock();
    s.price_ct = s_st.price_now_ct;
    s.act      = (c->mode == EM_MODE_OFF || !s_plan.n) ? EH_ACT_NONE : s_st.act_now;
    s.mode     = c->mode;
    s.acting   = s_st.acting;
    s.paused   = s_st.paused;
    unlock();
    em_hist_tick(now, &s);
}

/* -------------------------------- task --------------------------------- */

static void em_task(void *arg)
{
    (void)arg;
    load_profile_init();
    em_hist_init();
    bat_lock_init();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        int64_t now = (int64_t)time(NULL);
        /* The operator's locks first and in every state -- also before the
         * clock is set: a lock "until lifted" must not lapse after a restart. */
        static em_cfg_t lc;
        em_cfg_get(&lc);
        bat_lock_tick(now, lc.byd_ctrl);
        if (now < CLOCK_OK) continue;
        deye_force_tick(now);

        static em_cfg_t c;
        em_cfg_get(&c);
        lock(); s_st.mode = c.mode; unlock();
        hist_tick(now, &c);

        uint8_t req = s_req; s_req = 0;
        if (req & REQ_PRICES)   price_feed_request();
        if (req & REQ_FORECAST) pv_forecast_request();

        /* Feeds run in every mode: the page shows prices and the forecast
         * even with the manager off, and the profile keeps learning. */
        price_feed_tick(now);
        pv_forecast_tick(now);
        load_profile_tick(now);
        /* The Fronius storage module runs on the poll's connection to the
         * datalogger (modbus_tcp_borrow) and holds up that poll while it
         * reads. So it only talks to the Fronius when there is something to
         * control, a block of ours to keep or restore, or someone looks at
         * the card -- the BYD's SoC for display comes from the regular poll. */
        if (!c.byd_ctrl) fronius_storage_force_stop("BYD-Steuerung ausgeschaltet");
        fronius_storage_set_charge_cap(c.byd_ctrl ? c.byd_max_soc : 0);
        if ((c.byd_ctrl && c.mode == EM_MODE_AUTO) || s_own_byd || now < s_fronius_wanted ||
            fronius_storage_owed())
            fronius_storage_tick(now);

        if (c.mode == EM_MODE_OFF) {
            release("off");
            lock(); s_plan.n = 0; s_st.acting = false; snprintf(s_st.err, sizeof(s_st.err), "Energiemanager aus"); unlock();
            describe(now);
            continue;
        }

        /* Replan at every new quarter hour, on request, and when a feed
         * brought new data. */
        static EXT_RAM_BSS_ATTR price_set_t ps;
        static EXT_RAM_BSS_ATTR pv_fc_t     pv;
        price_feed_get(&ps);
        pv_forecast_get(&pv);
        int64_t slot = em_slot_floor(now);
        if (slot != s_last_plan_slot || (req & REQ_PLAN) ||
            ps.fetched != s_price_seen || pv.fetched != s_pv_seen) {
            plan_now(now, &c);
            s_last_plan_slot = slot;
        }
        act(now, &c);
        describe(now);
    }
}

static void em_shutdown(void) { load_profile_save_now(); }

/* cJSON builds a node per value: parsing one feed answer is hundreds of
 * small allocations, which the IDF heap places in INTERNAL RAM (everything
 * under SPIRAM_MALLOC_ALWAYSINTERNAL). That is the memory WiFi (esp_hosted)
 * and TLS live on, so cJSON goes to PSRAM -- for the whole firmware, the
 * settings import included; none of it is time-critical. */
static void *cj_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void em_mgr_start(void)
{
    if (s_task) return;
    cJSON_Hooks hooks = { .malloc_fn = cj_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    s_lock = xSemaphoreCreateMutex();
    memset(&s_st, 0, sizeof(s_st));
    s_st.price_now_ct = s_st.ren_now_pct = s_st.solar_now_pct = s_st.wind_now_pct = NAN;
    s_st.pv_today_kwh = s_st.pv_tomorrow_kwh = s_st.deye_soc = NAN;
    esp_register_shutdown_handler(em_shutdown);
    /* 8 kB: the HTTPS fetches (mbedTLS) run on this task. */
    xTaskCreatePinnedToCore(em_task, "energy_mgr", 8192, NULL, 3, &s_task, 0);
}

void em_mgr_get_status(em_status_t *out)
{
    if (!out || !s_lock) return;
    lock(); *out = s_st; unlock();
}

void em_mgr_get_plan(em_plan_t *out)
{
    if (!out || !s_lock) return;
    lock(); *out = s_plan; unlock();
}

esp_err_t em_mgr_set_mode(uint8_t mode, char *err, size_t errlen)
{
    em_cfg_t c;
    em_cfg_get(&c);
    c.mode = mode;
    esp_err_t e = em_cfg_set(&c, err, errlen);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "mode -> %u", mode);
        s_req |= REQ_PLAN;
        if (s_task) xTaskNotifyGive(s_task);
    }
    return e;
}

void em_mgr_want_fronius(void)
{
    s_fronius_wanted = (int64_t)time(NULL) + 120;
}

void em_mgr_request(const char *what)
{
    if (!what) return;
    if (!strcmp(what, "prices"))        s_req |= REQ_PRICES | REQ_PLAN;
    else if (!strcmp(what, "forecast")) s_req |= REQ_FORECAST | REQ_PLAN;
    else if (!strcmp(what, "plan"))     s_req |= REQ_PLAN;
    else if (!strcmp(what, "all"))      s_req |= REQ_PRICES | REQ_FORECAST | REQ_PLAN;
    if (s_task) xTaskNotifyGive(s_task);
}

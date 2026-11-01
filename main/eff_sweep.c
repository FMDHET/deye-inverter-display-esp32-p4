#include "eff_sweep.h"
#include "deye_ctrl.h"
#include "modbus_tcp.h"      /* the grid meter, for the cross-check */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "eff_sweep";

/* Everything below is guarded by s_lock: the task ticks it, the web handler
 * starts, stops and reads it. A mutex, not a spinlock -- the tick calls into
 * deye_ctrl, which writes NVS. */
static eff_sweep_status_t s_st;
static SemaphoreHandle_t  s_lock;
static TaskHandle_t       s_task;
static volatile bool      s_stop_req;

static uint32_t s_t0_ms;            /* sweep start                           */
static uint32_t s_step_t0_ms;       /* current step's power was applied      */
static uint32_t s_pause_until_ms;
static uint32_t s_offline_ms;       /* Deye silent since (0 = it answers)    */
static bool     s_applied;          /* deye_ctrl holds a mode we put there   */
static uint8_t  s_exp_mode;         /* ... which one                         */
static int      s_exp_power;        /* ... at which user power               */
static int      s_user_power0;      /* the slider value before the sweep     */
static uint32_t s_writes0;          /* deye_ctrl's wear counter at the start */
static uint32_t s_last_round;       /* last poll round taken into a step     */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void     lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void     unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* The clamp deye_ctrl will apply: the sweep's own floor for a forced step,
 * the normal range for Normal (whose power is only the slider value). */
static int clamp_power(int w, int floor_w)
{
    return w < floor_w ? floor_w : (w > DEYE_POWER_MAX ? DEYE_POWER_MAX : w);
}

/* Put a mode or a power into deye_ctrl and remember what we expect it to
 * hold, so a change from anywhere else can be told apart from our own. */
static void put(uint8_t mode, int power_w)
{
    if (mode == DEYE_MODE_NORMAL) {
        power_w = clamp_power(power_w, DEYE_POWER_MIN);
        deye_ctrl_apply(DEYE_MODE_NORMAL, power_w);
    } else {
        power_w = clamp_power(power_w, DEYE_POWER_FLOOR);
        if (s_applied && deye_ctrl_get_mode() == (deye_mode_t)mode)
            deye_ctrl_retune(power_w);
        else
            deye_ctrl_apply_sweep((deye_mode_t)mode, power_w);
    }
    s_applied   = true;
    s_exp_mode  = mode;
    s_exp_power = power_w;
}

/* Mark the steps of `mode` from `from` on as skipped, as far as they run. */
static void skip_mode(int from, uint8_t mode)
{
    for (int i = from; i < s_st.nsteps && s_st.step[i].mode == mode; i++)
        if (s_st.step[i].state == STEP_PENDING) s_st.step[i].state = STEP_SKIPPED;
}

/* End the sweep. `normal` writes Normal -- but only over a mode the sweep put
 * there itself: after an outside change the inverter is the user's again. */
static void finish(uint8_t state, const char *why, bool normal)
{
    for (int i = s_st.cur; i < s_st.nsteps; i++) {
        eff_sweep_step_t *p = &s_st.step[i];
        if (p->state == STEP_MEASURE && p->n) p->state = STEP_DONE;   /* keep it */
        else if (p->state != STEP_DONE)       p->state = STEP_SKIPPED;
    }
    if (normal && s_applied && deye_ctrl_get_mode() != DEYE_MODE_NORMAL)
        deye_ctrl_apply(DEYE_MODE_NORMAL, s_user_power0);
    deye_ctrl_status_t cs;
    deye_ctrl_get_status(&cs);
    s_st.writes  = cs.writes - s_writes0;     /* the closing Normal is wear too */
    s_applied    = false;
    s_st.paused  = false;
    s_st.state   = state;
    s_st.step_s  = 0;
    snprintf(s_st.why, sizeof(s_st.why), "%s", why);
    ESP_LOGW(TAG, "sweep %s: %s", state == SWEEP_DONE ? "done" : "aborted", why);
}

/* The current step's part of a tick. True when it handed over to the next
 * step, which then starts in the same second -- otherwise every step would
 * last one tick longer than asked, 38 s over a default sweep. */
static bool step_tick(uint32_t now, const mb_deye_live_t *l, bool online,
                      const deye_ctrl_status_t *cs, bool grid_ok, float grid_w)
{
    eff_sweep_step_t *p = &s_st.step[s_st.cur];
    const eff_sweep_req_t *rq = &s_st.req;
    bool    dis   = p->mode == DEYE_MODE_FORCE_DISCHARGE;
    uint8_t soc   = (uint8_t)(l->bat_soc + 0.5f);
    bool    limit = dis ? soc <= rq->soc_min : soc >= rq->soc_max;
    s_st.step_s   = p->state == STEP_PENDING ? 0 : (now - s_step_t0_ms) / 1000u;

    switch (p->state) {
    case STEP_PENDING:
        if (!online) return false;                       /* wait for the Deye */
        if (limit) {
            skip_mode(s_st.cur, p->mode);
            while (s_st.cur < s_st.nsteps && s_st.step[s_st.cur].state == STEP_SKIPPED)
                s_st.cur++;
            ESP_LOGI(TAG, "SoC %u %% -> %s steps skipped", soc, dis ? "discharge" : "charge");
            return false;
        }
        if (s_applied && s_exp_mode != DEYE_MODE_NORMAL && s_exp_mode != p->mode) {
            /* Direction change: through Normal (see eff_sweep.h). */
            put(DEYE_MODE_NORMAL, s_user_power0);
            s_st.paused      = true;
            s_pause_until_ms = now + SWEEP_PAUSE_S * 1000u;
            return false;
        }
        put(p->mode, p->target_w);
        p->state     = STEP_SETTLE;
        p->soc0      = soc;
        s_step_t0_ms = now;
        s_st.step_s  = 0;
        ESP_LOGI(TAG, "step %u/%u: %s %u W", s_st.cur + 1, s_st.nsteps,
                 dis ? "discharge" : "charge", p->target_w);
        return false;

    case STEP_SETTLE:
        if (limit) {                                /* nothing measured yet */
            p->state = STEP_SKIPPED;
            skip_mode(s_st.cur, p->mode);
            while (s_st.cur < s_st.nsteps && s_st.step[s_st.cur].state == STEP_SKIPPED)
                s_st.cur++;
            return false;
        }
        if (now - s_step_t0_ms >= rq->settle_s * 1000u) {
            /* By now deye_ctrl has written and read back the step's power.
             * A step whose power the inverter did not take measures some
             * other power -- worse than no step. */
            if (cs->failed) {
                finish(SWEEP_ABORTED, "Wechselrichter hat die Leistung nicht best\u00e4tigt", true);
                return false;
            }
            p->state     = STEP_MEASURE;
            p->soc0      = soc;
            s_last_round = l->rounds;       /* rounds from the settling don't count */
        }
        return false;

    case STEP_MEASURE:
        if (online && l->rounds != s_last_round) {
            s_last_round = l->rounds;
            p->soc1 = soc;
            /* Only rounds the curve counted too (steady, plausible), and only
             * in the step's direction -- a zero crossing is no measurement. */
            if (l->eff_ok && (dis ? l->bat_p > 0.0f : l->bat_p < 0.0f)) {
                float dc = l->bat_p + l->pv_total;   /* + = feeds the inverter */
                float ac = -l->inv_total;            /* + = delivers on AC     */
                p->in_sum  += dis ? dc : -ac;
                p->out_sum += dis ? (ac > 0.0f ? ac : 0.0f) : -dc;
                p->ac_sum  += ac;
                p->pv_sum  += l->pv_total;
                p->n++;
                if (grid_ok) {
                    p->grid_sum += grid_w;
                    p->ac_g_sum += ac;
                    p->n_grid++;
                }
            }
        }
        if (limit || now - s_step_t0_ms >= rq->dwell_s * 1000u) {
            p->state = STEP_DONE;
            s_st.cur++;
            if (limit) {
                skip_mode(s_st.cur, p->mode);
                while (s_st.cur < s_st.nsteps && s_st.step[s_st.cur].state == STEP_SKIPPED)
                    s_st.cur++;
            }
            return true;                            /* next step in this same second */
        }
        return false;

    default:
        s_st.cur++;                                 /* done/skipped: move on */
        return true;
    }
}

/* One second of the sweep. Pure apart from deye_ctrl: the live values and
 * the grid reading are handed in, so the host test can drive it. */
static void sweep_tick(uint32_t now, const mb_deye_live_t *l, bool grid_ok, float grid_w)
{
    if (s_st.state != SWEEP_RUNNING) { s_stop_req = false; return; }

    deye_ctrl_status_t cs;
    deye_ctrl_get_status(&cs);
    s_st.elapsed_s = (now - s_t0_ms) / 1000u;
    s_st.writes    = cs.writes - s_writes0;

    if (s_stop_req) {
        s_stop_req = false;
        finish(SWEEP_ABORTED, "abgebrochen", true);
        return;
    }
    if (s_applied && (deye_ctrl_get_mode() != (deye_mode_t)s_exp_mode ||
                      deye_ctrl_get_user_power() != s_exp_power)) {
        finish(SWEEP_ABORTED, "Betriebsart oder Leistung von außen geändert", false);
        return;
    }

    bool online = l->valid && l->online && l->age_ms < 10000;
    if (online) {
        s_offline_ms = 0;
    } else if (!s_offline_ms) {
        s_offline_ms = now ? now : 1;
    } else if (now - s_offline_ms >= SWEEP_OFFLINE_S * 1000u) {
        finish(SWEEP_ABORTED, "Deye antwortet nicht mehr", true);
        return;
    }

    if (s_st.paused) {
        if ((int32_t)(now - s_pause_until_ms) < 0) return;
        s_st.paused = false;
    }
    for (int pass = 0; pass < 2; pass++) {
        if (s_st.cur >= s_st.nsteps) {
            finish(SWEEP_DONE, "fertig", true);
            return;
        }
        if (!step_tick(now, l, online, &cs, grid_ok, grid_w)) return;
    }
}

/* Sort ascending, drop duplicates. Returns the new count. */
static int sort_powers(uint16_t *p, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && p[j - 1] > p[j]; j--) {
            uint16_t t = p[j]; p[j] = p[j - 1]; p[j - 1] = t;
        }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!m || p[m - 1] != p[i]) p[m++] = p[i];
    return m;
}

static esp_err_t refuse(char *err, size_t len, const char *why)
{
    if (err && len) snprintf(err, len, "%s", why);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t eff_sweep_start(const eff_sweep_req_t *in, char *err, size_t errlen)
{
    if (!in) return refuse(err, errlen, "keine Angaben");
    eff_sweep_req_t rq = *in;

    if (!(rq.dirs & (SWEEP_DIR_DIS | SWEEP_DIR_CHG)))
        return refuse(err, errlen, "Laden, Entladen oder beides wählen");
    if (rq.npow < 1 || rq.npow > SWEEP_MAX_POWERS)
        return refuse(err, errlen, "1 bis 20 Leistungsstufen");
    for (int i = 0; i < rq.npow; i++)
        if (rq.pow[i] < DEYE_POWER_FLOOR || rq.pow[i] > DEYE_POWER_MAX)
            return refuse(err, errlen, "Leistungen von 250 bis 20000 W");
    rq.npow = (uint8_t)sort_powers(rq.pow, rq.npow);
    /* 30 s is enough for an electrical operating point: the inverter takes
     * its new power within a few seconds, the battery register lags by about
     * three rounds, and the steadiness test drops those rounds anyway. 20 s
     * of measuring are ten poll rounds. */
    if (rq.dwell_s < 30 || rq.dwell_s > 1800)
        return refuse(err, errlen, "Dauer je Stufe 30 s bis 30 min");
    if (rq.settle_s < 10 || rq.settle_s + 20 > rq.dwell_s)
        return refuse(err, errlen, "Einschwingzeit ab 10 s und mindestens 20 s kürzer als die Stufe");
    if (rq.soc_min < 5 || rq.soc_max > 100 || rq.soc_min + 10 > rq.soc_max)
        return refuse(err, errlen, "SoC-Grenzen: unten ab 5 %, oben bis 100 %, 10 % Abstand");
    if ((uint32_t)rq.npow * rq.dwell_s > SWEEP_DIR_MAX_S)
        return refuse(err, errlen, "Eine Richtung darf höchstens 110 min dauern (2-h-Grenze der Zwangsmodi)");

    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    if (!(l.valid && l.online && l.age_ms < 10000))
        return refuse(err, errlen, "Deye antwortet nicht");

    lock();
    if (s_st.state == SWEEP_RUNNING) { unlock(); return refuse(err, errlen, "läuft schon"); }
    if (deye_ctrl_get_mode() != DEYE_MODE_NORMAL) {
        unlock();
        return refuse(err, errlen, "Erst die laufende Zwangsladung/-entladung beenden");
    }
    float soc = l.bat_soc;
    bool can_dis = (rq.dirs & SWEEP_DIR_DIS) && soc > rq.soc_min;
    bool can_chg = (rq.dirs & SWEEP_DIR_CHG) && soc < rq.soc_max;
    if (!can_dis && !can_chg) {
        unlock();
        return refuse(err, errlen, "SoC liegt schon an der Grenze der gewählten Richtung");
    }

    memset(&s_st, 0, sizeof(s_st));
    s_st.req = rq;
    /* Both directions: the one with more room first, so the second one
     * finds the battery back in the middle. */
    uint8_t order[2] = { DEYE_MODE_FORCE_DISCHARGE, DEYE_MODE_FORCE_CHARGE };
    if (soc < 50.0f) { order[0] = DEYE_MODE_FORCE_CHARGE; order[1] = DEYE_MODE_FORCE_DISCHARGE; }
    for (int d = 0; d < 2; d++) {
        uint8_t m = order[d];
        if (!(rq.dirs & (m == DEYE_MODE_FORCE_DISCHARGE ? SWEEP_DIR_DIS : SWEEP_DIR_CHG))) continue;
        for (int i = 0; i < rq.npow; i++) {
            eff_sweep_step_t *p = &s_st.step[s_st.nsteps++];
            p->mode     = m;
            p->target_w = rq.pow[i];
        }
    }
    deye_ctrl_status_t cs;
    deye_ctrl_get_status(&cs);
    s_writes0     = cs.writes;
    s_user_power0 = deye_ctrl_get_user_power();
    s_t0_ms       = now_ms();
    s_offline_ms  = 0;
    s_applied     = false;
    s_stop_req    = false;
    time_t t = time(NULL);
    s_st.start_unix = t > 1700000000 ? (uint32_t)t : 0;
    s_st.state      = SWEEP_RUNNING;
    snprintf(s_st.why, sizeof(s_st.why), "läuft");
    unlock();

    ESP_LOGW(TAG, "sweep started: %u steps, %u s each (%u s settling), SoC %u..%u %%",
             s_st.nsteps, rq.dwell_s, rq.settle_s, rq.soc_min, rq.soc_max);
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

void eff_sweep_stop(void)
{
    s_stop_req = true;
    if (s_task) xTaskNotifyGive(s_task);
}

bool eff_sweep_running(void) { return s_st.state == SWEEP_RUNNING; }

void eff_sweep_get(eff_sweep_status_t *out)
{
    if (!out) return;
    lock();
    *out = s_st;
    unlock();
    if (out->state != SWEEP_RUNNING) { out->left_s = 0; return; }
    /* What is left: the rest of the current step, every pending step, and a
     * pause where the direction still changes. An estimate -- a SoC limit
     * ends a direction early. */
    uint32_t left = 0;
    for (int i = out->cur; i < out->nsteps; i++) {
        const eff_sweep_step_t *p = &out->step[i];
        if (p->state == STEP_SKIPPED || p->state == STEP_DONE) continue;
        left += (i == out->cur && p->state != STEP_PENDING)
                ? (out->step_s < out->req.dwell_s ? out->req.dwell_s - out->step_s : 0)
                : out->req.dwell_s;
        if (i > out->cur && out->step[i - 1].mode != p->mode) left += SWEEP_PAUSE_S;
    }
    out->left_s = left;
}

static void sweep_task(void *arg)
{
    (void)arg;
    static mb_deye_live_t l;             /* off the task stack */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, eff_sweep_running() || s_stop_req
                                 ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
        modbus_rtu_get_deye_live(&l);
        float g = 0.0f;
        bool  gok = modbus_tcp_grid_w_fresh(&g, MB_GRID_MAX_AGE_MS);
        lock();
        sweep_tick(now_ms(), &l, gok, g);
        unlock();
    }
}

void eff_sweep_init(void)
{
    if (s_task) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(sweep_task, "eff_sweep", 3072, NULL, 3, &s_task, 0);
}

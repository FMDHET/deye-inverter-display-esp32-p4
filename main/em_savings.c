#include "em_savings.h"
#include "em_hist.h"
#include "em_types.h"
#include "bat_cost.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "em_sav";

static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static em_sav_day_t  s_days[EM_SAV_DAYS];
static int           s_n;
static int64_t       s_done_slot = -1;
static eh_slot_t    *s_buf;              /* EM_SAV_DAYS x 100 slots, PSRAM (~140 kB) */
#define BUF_SLOTS    (EM_SAV_DAYS * 100)

static int64_t local_midnight(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

/* Midnight of the day after the one starting at m (23, 24 or 25 h later). */
static int64_t next_midnight(int64_t m) { return local_midnight(m + 26 * 3600); }

void em_savings_tick(int64_t now, const em_cfg_t *c)
{
    int64_t slot = em_slot_floor(now);
    if (slot == s_done_slot) return;
    if (!s_buf) s_buf = heap_caps_malloc(sizeof(eh_slot_t) * BUF_SLOTS, MALLOC_CAP_SPIRAM);
    if (!s_buf) return;
    s_done_slot = slot;

    /* the oldest day's midnight */
    int64_t m = local_midnight(now);
    for (int k = 1; k < EM_SAV_DAYS; k++) m = local_midnight(m - 12 * 3600);
    int64_t start = m;
    int total = (int)((now - start) / EM_SLOT_S) + 1;
    if (total > BUF_SLOTS) total = BUF_SLOTS;
    em_hist_range(start, total, s_buf);

    /* the value of a DC kWh in each battery today (bat_cost) */
    bat_cost_status_t bc;
    bat_cost_get(&bc);
    float v_deye = bc.deye.valid ? bc.deye.out_ct * bc.deye.eta_d : NAN;
    float v_byd  = bc.byd.valid  ? bc.byd.out_ct  * bc.byd.eta_d  : NAN;

    static em_sav_day_t days[EM_SAV_DAYS];
    int nd = 0;
    for (int64_t d0 = start; d0 <= now && nd < EM_SAV_DAYS; d0 = next_midnight(d0)) {
        em_sav_day_t *o = &days[nd++];
        memset(o, 0, sizeof(*o));
        o->t0 = d0;
        int64_t d1 = next_midnight(d0);
        int i0 = (int)((d0 - start) / EM_SLOT_S), i1 = (int)((d1 - start) / EM_SLOT_S);
        if (i1 > total) i1 = total;
        float sd0 = NAN, sd1 = NAN, sb0 = NAN, sb1 = NAN;
        for (int i = i0; i < i1; i++) {
            const eh_slot_t *s = &s_buf[i];
            if (!s->valid) continue;
            if (isfinite(s->deye_soc)) { if (isnan(sd0)) sd0 = s->deye_soc; sd1 = s->deye_soc; }
            if (isfinite(s->byd_soc))  { if (isnan(sb0)) sb0 = s->byd_soc;  sb1 = s->byd_soc; }
            if (!isfinite(s->grid_w) || !isfinite(s->pv_w) || !isfinite(s->house_w) || !isfinite(s->price_ct))
                continue;
            float fi = (c->feedin_neg0 && s->price_ct < 0) ? 0.0f : c->feedin_ct;
            float g = s->grid_w / 1000.0f, nb = (s->house_w - s->pv_w) / 1000.0f;
            o->cost_ct += (g > 0 ? g * s->price_ct : g * fi) * 0.25f;
            o->base_ct += (nb > 0 ? nb * s->price_ct : nb * fi) * 0.25f;
            if (g > 0) o->imp_kwh += g * 0.25f; else o->exp_kwh -= g * 0.25f;
            o->pv_kwh    += (s->pv_w > 0 ? s->pv_w : 0) / 4000.0f;
            o->house_kwh += (s->house_w > 0 ? s->house_w : 0) / 4000.0f;
            o->slots++;
        }
        /* SoC at the end of the first and the last slot recorded: the
         * change of stored energy, at what it cost */
        if (isfinite(sd0) && isfinite(sd1) && isfinite(v_deye) && c->deye_kwh > 0)
            o->stored_ct += (sd1 - sd0) / 100.0f * c->deye_kwh * v_deye;
        if (isfinite(sb0) && isfinite(sb1) && isfinite(v_byd) && c->byd_kwh > 0)
            o->stored_ct += (sb1 - sb0) / 100.0f * c->byd_kwh * v_byd;
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_days, days, sizeof(days));
    s_n = nd;
    portEXIT_CRITICAL(&s_mux);
    if (nd) {
        const em_sav_day_t *t = &days[nd - 1];
        ESP_LOGD(TAG, "today: cost %.0f ct, without batteries %.0f ct, stored %.0f ct (%u slots)",
                 t->cost_ct, t->base_ct, t->stored_ct, t->slots);
    }
}

int em_savings_get(em_sav_day_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_n < max ? s_n : max;
    memcpy(out, s_days, sizeof(em_sav_day_t) * n);
    portEXIT_CRITICAL(&s_mux);
    return n;
}

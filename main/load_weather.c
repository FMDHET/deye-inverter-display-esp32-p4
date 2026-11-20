#include "load_weather.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------ the fit --------------------------------- */

void lw_fit(const lw_point_t *p, int n, lw_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->alpha = 1.0f;
    m->n = n;
    if (n < 2) return;
    double sx = 0, sy = 0;
    for (int i = 0; i < n; i++) { sx += p[i].hdd; sy += p[i].ratio; }
    double mx = sx / n, my = sy / n, sxx = 0, sxy = 0, syy = 0;
    for (int i = 0; i < n; i++) {
        double dx = p[i].hdd - mx, dy = p[i].ratio - my;
        sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
    }
    if (sxx < 1e-6 || syy < 1e-9) { m->alpha = (float)my; return; }
    double b = sxy / sxx;
    m->beta  = (float)b;
    m->alpha = (float)(my - b * mx);
    m->r2    = (float)(sxy * sxy / (sxx * syy));
    /* Too few days, no heating effect, or noise: not used. A spread of the
     * temperatures below 2 K says nothing either (a mild month). */
    m->ok = n >= LW_MIN_DAYS && b > 0 && m->r2 >= LW_MIN_R2 && sqrt(sxx / n) >= 2.0;
}

float lw_factor(const lw_model_t *m, float t)
{
    if (!m->ok || !isfinite(t)) return 1.0f;
    float hdd = LW_HDD_BASE - t;
    if (hdd < 0) hdd = 0;
    float f = m->alpha + m->beta * hdd;
    if (f < LW_FACTOR_MIN) f = LW_FACTOR_MIN;
    if (f > LW_FACTOR_MAX) f = LW_FACTOR_MAX;
    return f;
}

#ifndef LW_HOST_TEST   /* --------------- device side ------------------ */

#include "https_get.h"
#include "load_hist.h"
#include "load_profile.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "load_wx";

#define REFRESH_S  (6 * 3600)
#define RETRY_S    (30 * 60)
#define BUF_CAP    6144

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static load_weather_t s_st;
static int64_t        s_next;

static int32_t civil_days(int y, int m, int d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (uint32_t)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (uint32_t)d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

int32_t load_weather_day_key(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    return civil_days(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

void load_weather_get(load_weather_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

float load_weather_factor(int32_t day)
{
    float f = 1.0f;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LW_FC_DAYS; i++) if (s_st.day[i] == day) f = s_st.factor[i];
    portEXIT_CRITICAL(&s_mux);
    return f;
}

static void set_err(const char *e)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.err, sizeof(s_st.err), "%s", e);
    portEXIT_CRITICAL(&s_mux);
}

/* Fit the stored days against the fetched temperatures, then the factors
 * for the forecast days. days[]/temps[] as Open-Meteo gave them. */
static void fit_and_publish(const int32_t *days, const float *temps, int nd, int32_t today, int64_t now)
{
    static load_profile_t lp;                /* ~0.8 kB: off the task stack */
    load_profile_get(&lp);
    double e_prof[2] = { 0, 0 };
    int    k_prof[2] = { 0, 0 };
    for (int s = 0; s < LP_SLOTS; s++) {
        if (isfinite(lp.wd_kw[s])) { e_prof[0] += lp.wd_kw[s] * 0.25; k_prof[0]++; }
        if (isfinite(lp.we_kw[s])) { e_prof[1] += lp.we_kw[s] * 0.25; k_prof[1]++; }
    }
    for (int w = 0; w < 2; w++) if (k_prof[w] < 90) e_prof[w] = 0;   /* profile not whole yet */

    static lw_point_t pts[LW_PAST_DAYS];
    static lh_day_t   d;
    int n = 0;
    double e_sum = 0;
    for (int k = 0; n < LW_PAST_DAYS && load_hist_get(k, &d); k++) {
        if (d.day >= today) continue;
        float t = NAN;
        for (int i = 0; i < nd; i++) if (days[i] == d.day) { t = temps[i]; break; }
        if (!isfinite(t)) { if (d.day < days[0]) break; continue; }
        int cnt = 0; double wh = 0;
        for (int s = 0; s < LH_SLOTS; s++) if (d.w[s] != LH_NONE) { wh += d.w[s] * 0.25; cnt++; }
        if (cnt < 88) continue;                  /* a day with gaps says little */
        double e = wh / 1000.0 * LH_SLOTS / cnt;
        int we = (d.wday == 0 || d.wday == 6);
        if (e_prof[we] <= 0.1) continue;
        pts[n].hdd   = t < LW_HDD_BASE ? LW_HDD_BASE - t : 0.0f;
        pts[n].ratio = (float)(e / e_prof[we]);
        e_sum += e_prof[we];
        n++;
    }
    lw_model_t m;
    lw_fit(pts, n, &m);

    load_weather_t st;
    load_weather_get(&st);
    st.m = m;
    st.kwh_per_k = n ? (float)(m.beta * e_sum / n) : 0.0f;
    st.fetched = now;
    st.err[0] = '\0';
    for (int i = 0; i < LW_FC_DAYS; i++) { st.day[i] = 0; st.t_mean[i] = NAN; st.factor[i] = 1.0f; }
    int j = 0;
    for (int i = 0; i < nd && j < LW_FC_DAYS; i++) {
        if (days[i] < today) continue;
        st.day[j] = days[i];
        st.t_mean[j] = temps[i];
        st.factor[j] = lw_factor(&m, temps[i]);
        j++;
    }
    portENTER_CRITICAL(&s_mux);
    s_st = st;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%d days: ratio = %.3f + %.4f * HDD, r2 %.2f -> %s; today %.1f C x%.2f, tomorrow %.1f C x%.2f",
             n, m.alpha, m.beta, m.r2, m.ok ? "used" : "not used",
             st.t_mean[0], st.factor[0], st.t_mean[1], st.factor[1]);
}

void load_weather_tick(int64_t now, const em_cfg_t *c)
{
    if (now < s_next) return;
    if (c->lat == 0.0f && c->lon == 0.0f) { s_next = now + RETRY_S; set_err("Standort fehlt"); return; }
    s_next = now + RETRY_S;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&daily=temperature_2m_mean&past_days=%d&forecast_days=%d&timezone=Europe%%2FBerlin",
             c->lat, c->lon, LW_PAST_DAYS, LW_FC_DAYS);
    char *buf = heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { set_err("kein Speicher"); return; }
    size_t len = 0; int stc = 0;
    esp_err_t e = https_get(&(https_req_t){ .url = url }, buf, BUF_CAP, &len, &stc);
    if (e != ESP_OK) {
        char m[64];
        snprintf(m, sizeof(m), "Open-Meteo: %s (HTTP %d)", esp_err_to_name(e), stc);
        set_err(m);
        ESP_LOGW(TAG, "%s", m);
        free(buf);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const cJSON *tm = cJSON_GetObjectItem(daily, "time"), *tv = cJSON_GetObjectItem(daily, "temperature_2m_mean");
    int nd = cJSON_IsArray(tm) && cJSON_IsArray(tv) ? cJSON_GetArraySize(tm) : 0;
    if (nd > LW_PAST_DAYS + LW_FC_DAYS + 2) nd = LW_PAST_DAYS + LW_FC_DAYS + 2;
    static int32_t days[LW_PAST_DAYS + LW_FC_DAYS + 2];
    static float   temps[LW_PAST_DAYS + LW_FC_DAYS + 2];
    int k = 0;
    for (int i = 0; i < nd; i++) {
        const cJSON *ds = cJSON_GetArrayItem(tm, i), *t = cJSON_GetArrayItem(tv, i);
        int y, mo, dd;
        if (!cJSON_IsString(ds) || sscanf(ds->valuestring, "%d-%d-%d", &y, &mo, &dd) != 3) continue;
        days[k]  = civil_days(y, mo, dd);
        temps[k] = cJSON_IsNumber(t) ? (float)t->valuedouble : NAN;
        k++;
    }
    cJSON_Delete(root);
    if (k < LW_FC_DAYS) { set_err("Open-Meteo: Antwort ohne Tagestemperaturen"); return; }
    fit_and_publish(days, temps, k, load_weather_day_key(now), now);
    s_next = now + REFRESH_S;
}

#endif /* LW_HOST_TEST */

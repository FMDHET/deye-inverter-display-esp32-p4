#include "deye_web.h"
#include "modbus_rtu.h"
#include "deye_ctrl.h"
#include "webauth.h"
#include "modbus_gw.h"
#include "modbus_tcp.h"
#include "fronius_meter.h"
#include "eff_sweep.h"
#include "nvs_store.h"
#include "em_cfg.h"
#include "energy_mgr.h"
#include "price_feed.h"
#include "pv_forecast.h"
#include "load_profile.h"
#include "load_hist.h"
#include "em_hist.h"
#include "bat_lock.h"
#include "deye_force.h"
#include "fronius_storage.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "lwip/inet.h"

#include <stdarg.h>
#include <stddef.h>
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "deye_web";

/* deye.html via EMBED_TXTFILES. */
extern const char deye_html_start[] asm("_binary_deye_html_start");

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, deye_html_start, HTTPD_RESP_USE_STRLEN);
}

/* Parse an integer query parameter, or `def` if absent/invalid. */
static int qparam_int(httpd_req_t *req, const char *key, int def)
{
    char q[96], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return def;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK)   return def;
    return atoi(v);
}

/* GET /deye/read?addr=&count= -> {"ok":true,"addr":N,"vals":[...]} */
static esp_err_t read_handler(httpd_req_t *req)
{
    int addr  = qparam_int(req, "addr",  -1);
    int count = qparam_int(req, "count",  1);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (addr < 0 || addr > 65535 || count < 1 || count > 64)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"bad args\"}");

    uint16_t vals[64];
    int rc = modbus_rtu_deye_read((uint16_t)addr, (uint16_t)count, vals);

    char buf[760];
    int o = 0;
    if (rc != 0) {
        o += snprintf(buf + o, sizeof(buf) - o,
                      "{\"ok\":false,\"rc\":%d,\"addr\":%d}", rc, addr);
    } else {
        o += snprintf(buf + o, sizeof(buf) - o,
                      "{\"ok\":true,\"addr\":%d,\"data\":[", addr);
        for (int i = 0; i < count && o < (int)sizeof(buf) - 12; i++)
            o += snprintf(buf + o, sizeof(buf) - o, "%s%u", i ? "," : "", vals[i]);
        o += snprintf(buf + o, sizeof(buf) - o, "]}");
    }
    return httpd_resp_sendstr(req, buf);
}

/* GET /deye/write?addr=&val= -> {"ok":bool,"rc":N} (FC06 single register) */
static esp_err_t write_handler(httpd_req_t *req)
{
    /* Writing arbitrary registers into the inverter -- the most dangerous
     * endpoint on the device. */
    if (!web_auth_ok(req)) return ESP_OK;

    int addr = qparam_int(req, "addr", -1);
    int val  = qparam_int(req, "val",  -1);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (addr < 0 || addr > 65535 || val < 0 || val > 65535)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"bad args\"}");

    int rc = modbus_rtu_deye_write((uint16_t)addr, (uint16_t)val);
    ESP_LOGW(TAG, "write reg %d = %d -> rc=%d", addr, val, rc);

    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"rc\":%d,\"addr\":%d,\"val\":%d}",
             rc == 0 ? "true" : "false", rc, addr, val);
    return httpd_resp_sendstr(req, buf);
}

/* ------------------- live values the Deye reports -------------------- */

/* esp_http_server dispatches from a single task, so handlers never run
 * concurrently -- a static buffer keeps ~1.5 kB off the (6 kB) server stack. */
#define LIVE_CAP 2048
static EXT_RAM_BSS_ATTR char s_live_json[LIVE_CAP];

/* Append, saturating at cap-1. snprintf returns what it WOULD have written, so
 * a bare `o += snprintf(...)` lets `o` run past `cap` and turns the next
 * `cap - o` into a huge size_t. Same helper as meter_web.c's jcat(). */
static int jcat(char *buf, size_t cap, int o, const char *fmt, ...)
{
    if (o < 0 || (size_t)o >= cap) return (int)cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + o, cap - (size_t)o, fmt, ap);
    va_end(ap);
    if (n < 0) return o;
    o += n;
    return ((size_t)o >= cap) ? (int)cap - 1 : o;
}

/* GET /api/deye/live -> the inverter's own measurements, already scaled.
 *
 * `blocks` is the bitmask of register ranges that answered in the last poll
 * round; the page uses it to mark a group as stale instead of showing the
 * carried-over values as if they were current. */
static esp_err_t live_handler(httpd_req_t *req)
{
    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);

    int o = 0;
    o = jcat(s_live_json, LIVE_CAP, o,
             "{\"valid\":%d,\"online\":%d,\"blocks\":%u,\"age\":%u,",
             l.valid ? 1 : 0, l.online ? 1 : 0, l.blocks, (unsigned)l.age_ms);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"bat\":{\"soc\":%.0f,\"p\":%.0f,\"v\":%.2f,\"i\":%.2f,\"t\":%.1f},",
             l.bat_soc, l.bat_p, l.bat_v, l.bat_i, l.bat_temp);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"pv\":{\"total\":%.0f,\"p\":[%.0f,%.0f,%.0f,%.0f],"
             "\"v\":[%.1f,%.1f,%.1f,%.1f],\"i\":[%.1f,%.1f,%.1f,%.1f]},",
             l.pv_total, l.pv_p[0], l.pv_p[1], l.pv_p[2], l.pv_p[3],
             l.pv_v[0], l.pv_v[1], l.pv_v[2], l.pv_v[3],
             l.pv_i[0], l.pv_i[1], l.pv_i[2], l.pv_i[3]);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"inv\":{\"total\":%.0f,\"freq\":%.2f,\"p\":[%.0f,%.0f,%.0f],"
             "\"v\":[%.1f,%.1f,%.1f],\"i\":[%.2f,%.2f,%.2f]},",
             l.inv_total, l.inv_freq, l.inv_p[0], l.inv_p[1], l.inv_p[2],
             l.inv_v[0], l.inv_v[1], l.inv_v[2],
             l.inv_i[0], l.inv_i[1], l.inv_i[2]);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"load\":{\"total\":%.0f,\"freq\":%.2f,\"p\":[%.0f,%.0f,%.0f],"
             "\"v\":[%.1f,%.1f,%.1f],\"i\":[%.2f,%.2f,%.2f],"
             "\"ups_total\":%.0f,\"ups\":[%.0f,%.0f,%.0f]},",
             l.load_total, l.load_freq, l.load_p[0], l.load_p[1], l.load_p[2],
             l.load_v[0], l.load_v[1], l.load_v[2],
             l.load_i[0], l.load_i[1], l.load_i[2],
             l.ups_total, l.ups_p[0], l.ups_p[1], l.ups_p[2]);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"grid\":{\"freq\":%.2f,\"v\":[%.1f,%.1f,%.1f],"
             "\"ct_total\":%.0f,\"ct\":[%.0f,%.0f,%.0f],"
             "\"total\":%.0f,\"p\":[%.0f,%.0f,%.0f],"
             "\"inner_total\":%.0f,\"inner\":[%.0f,%.0f,%.0f]},",
             l.grid_freq, l.grid_v[0], l.grid_v[1], l.grid_v[2],
             l.grid_ct_total, l.grid_ct_p[0], l.grid_ct_p[1], l.grid_ct_p[2],
             l.grid_total, l.grid_p[0], l.grid_p[1], l.grid_p[2],
             l.grid_inner_total, l.grid_inner_p[0], l.grid_inner_p[1], l.grid_inner_p[2]);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"eff\":{\"ok\":%d,\"since\":%u,\"dis_since\":%u,\"dis_full\":%d,\"full\":%u,"
             "\"dis_dc\":%.1f,\"dis_ac\":%.1f,\"chg_ac\":%.1f,\"chg_dc\":%.1f,"
             "\"lt\":{\"dis_dc\":%.0f,\"dis_ac\":%.0f,\"chg_ac\":%.0f,\"chg_dc\":%.0f,"
             "\"start\":%lld}},",
             l.eff_ok ? 1 : 0, (unsigned)l.eff_s, (unsigned)l.eff_dis_s,
             l.eff_dis_full ? 1 : 0, (unsigned)l.full_s,
             l.e_dis_dc, l.e_dis_ac, l.e_chg_ac, l.e_chg_dc,
             l.lt.dis_dc, l.lt.dis_ac, l.lt.chg_ac, l.lt.chg_dc, (long long)l.lt.start);
    if (l.ectr_ok)
        o = jcat(s_live_json, LIVE_CAP, o,
                 "\"ectr\":{\"chg_kwh\":%.1f,\"dis_kwh\":%.1f,\"age\":%u},",
                 l.bat_chg_kwh, l.bat_dis_kwh, (unsigned)l.ectr_age_ms);
    /* Battery mode: what is in force, how long it still has, and whether its
     * register writes actually verified -- a forced mode whose writes failed
     * used to look exactly like one that worked. */
    deye_ctrl_status_t cs;
    deye_ctrl_get_status(&cs);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"ctrl\":{\"mode\":%d,\"mode_name\":\"%s\",\"power\":%d,\"user_power\":%d,"
             "\"age\":%u,\"left\":%u,\"checked\":%u,\"failed\":%u,\"writes\":%u}}",
             (int)cs.mode, deye_ctrl_mode_name(cs.mode), cs.power_w, cs.user_power_w,
             (unsigned)cs.age_s, (unsigned)cs.left_s,
             (unsigned)cs.checked, (unsigned)cs.failed, (unsigned)cs.writes);
    (void)o;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s_live_json);
}

/* POST /api/deye/eff/reset -> start the efficiency statistic from zero. */
static esp_err_t eff_reset_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    modbus_rtu_reset_deye_eff();
    ESP_LOGI(TAG, "efficiency statistic reset");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":1}");
}

/* ---------------- efficiency by power + sweep ----------------
 * GET  /api/deye/eff/curve  -> the curve (mb_eff_curve_t) and the sweep.
 * POST /api/deye/eff/curve?cmd=reset
 *                          ?cmd=stop
 *                          ?cmd=sweep&dirs=1|2|3&p=1000,2000,..&dwell=S&settle=S&smin=%&smax=%
 *
 * Curve bins go out as [in_wh, out_wh, seconds], lower edges once in "lo";
 * sweep steps as means, so the page never divides by a count itself. */
#define CURVE_CAP 10240   /* 29 bins x 2 + up to 40 steps */
static EXT_RAM_BSS_ATTR char s_curve_json[CURVE_CAP];   /* same single-task argument as s_live_json */

static int curve_bins(char *b, int o, const char *key, const mb_eff_bin_t *bin, int n)
{
    o = jcat(b, CURVE_CAP, o, "\"%s\":[", key);
    for (int i = 0; i < n; i++)
        o = jcat(b, CURVE_CAP, o, "%s[%.1f,%.1f,%u]", i ? "," : "",
                 bin[i].in_wh, bin[i].out_wh, (unsigned)bin[i].s);
    return jcat(b, CURVE_CAP, o, "],");
}

static esp_err_t curve_get_handler(httpd_req_t *req)
{
    static EXT_RAM_BSS_ATTR mb_eff_curve_t c;          /* ~0.5 kB, off the server stack */
    static EXT_RAM_BSS_ATTR eff_sweep_status_t sw;     /* ~1.8 kB */
    modbus_rtu_get_eff_curve(&c);
    eff_sweep_get(&sw);
    char *b = s_curve_json;

    int o = jcat(b, CURVE_CAP, 0, "{\"start\":%u,\"idle_w\":%.0f,\"nvs_free\":%u,\"lo\":[",
                 (unsigned)c.start, (double)MB_EFF_IDLE_W, (unsigned)nvs_store_free_entries());
    for (int i = 0; i < MB_EFF_BINS; i++)
        o = jcat(b, CURVE_CAP, o, "%s%u", i ? "," : "", MB_EFF_BIN_LO[i]);
    o = jcat(b, CURVE_CAP, o, "],");
    o = curve_bins(b, o, "dis", c.dis, MB_EFF_BINS);
    o = curve_bins(b, o, "chg", c.chg, MB_EFF_BINS);
    o = jcat(b, CURVE_CAP, o, "\"idle\":[%.1f,%.1f,%u],",
             c.idle.in_wh, c.idle.out_wh, (unsigned)c.idle.s);

    o = jcat(b, CURVE_CAP, o,
             "\"sweep\":{\"state\":%u,\"why\":\"%s\",\"cur\":%u,\"n\":%u,\"paused\":%d,"
             "\"start\":%u,\"elapsed\":%u,\"step_s\":%u,\"left\":%u,\"writes\":%u,"
             "\"dirs\":%u,\"dwell\":%u,\"settle\":%u,\"smin\":%u,\"smax\":%u,\"steps\":[",
             sw.state, sw.why, sw.cur, sw.nsteps, sw.paused ? 1 : 0,
             (unsigned)sw.start_unix, (unsigned)sw.elapsed_s, (unsigned)sw.step_s,
             (unsigned)sw.left_s, (unsigned)sw.writes, sw.req.dirs, sw.req.dwell_s,
             sw.req.settle_s, sw.req.soc_min, sw.req.soc_max);
    for (int i = 0; i < sw.nsteps; i++) {
        const eff_sweep_step_t *p = &sw.step[i];
        float n = p->n ? (float)p->n : 1.0f, ng = p->n_grid ? (float)p->n_grid : 1.0f;
        o = jcat(b, CURVE_CAP, o,
                 "%s{\"m\":%u,\"st\":%u,\"t\":%u,\"n\":%u,\"ng\":%u,\"in\":%.1f,\"out\":%.1f,"
                 "\"ac\":%.1f,\"pv\":%.0f,\"grid\":%.1f,\"acg\":%.1f,\"soc0\":%u,\"soc1\":%u}",
                 i ? "," : "", p->mode, p->state, p->target_w, p->n, p->n_grid,
                 p->in_sum / n, p->out_sum / n, p->ac_sum / n, p->pv_sum / n,
                 p->grid_sum / ng, p->ac_g_sum / ng, p->soc0, p->soc1);
    }
    o = jcat(b, CURVE_CAP, o, "]}}");
    (void)o;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, b);
}

/* "1000,2000,5000" -> pow[], at most max entries. Returns the count, or -1
 * on anything that is not a list of plain numbers. httpd_query_key_value()
 * does NOT url-decode, so a browser's "%2C" for the comma is accepted too. */
static int parse_powers(const char *v, uint16_t *pow, int max)
{
    int n = 0;
    while (*v) {
        char *end = NULL;
        long w = strtol(v, &end, 10);
        if (end == v || w < 0 || w > 65535 || n >= max) return -1;
        pow[n++] = (uint16_t)w;
        v = end;
        if (*v == ',') v++;
        else if (v[0] == '%' && v[1] == '2' && (v[2] == 'C' || v[2] == 'c')) v += 3;
        else if (*v) return -1;
    }
    return n;
}

static esp_err_t curve_post_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    char q[400], cmd[12] = "", v[16];   /* 20 powers ~ 120 chars, encoded ~ 160 */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "cmd", cmd, sizeof(cmd)) != ESP_OK)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"cmd fehlt\"}");

    if (!strcmp(cmd, "reset")) {
        if (eff_sweep_running())
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"Durchlauf l\u00e4uft\"}");
        modbus_rtu_reset_eff_curve();
        ESP_LOGI(TAG, "efficiency curve reset");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (!strcmp(cmd, "stop")) {
        eff_sweep_stop();
        ESP_LOGI(TAG, "efficiency sweep stop requested");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (strcmp(cmd, "sweep"))
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"unbekanntes cmd\"}");

    eff_sweep_req_t rq = { 0 };
    char pl[240];
    int np = httpd_query_key_value(q, "p", pl, sizeof(pl)) == ESP_OK
             ? parse_powers(pl, rq.pow, SWEEP_MAX_POWERS) : -1;
#define QI(key, def) (httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK ? atoi(v) : (def))
    int dirs = QI("dirs", 3), dwell = QI("dwell", 30), settle = QI("settle", 10);
    int smin = QI("smin", 20), smax = QI("smax", 95);
#undef QI
    char err[100] = "";
    if (np < 1) {
        snprintf(err, sizeof(err), "Leistungsliste unlesbar (z. B. 1000,2000,5000)");
    } else if (dirs < 1 || dirs > 3 || dwell < 0 || dwell > 65535 || settle < 0 || settle > 65535 ||
               smin < 0 || smin > 100 || smax < 0 || smax > 100) {
        snprintf(err, sizeof(err), "Angaben au\u00dferhalb des Bereichs");
    } else {
        rq.npow = (uint8_t)np;   rq.dirs = (uint8_t)dirs;
        rq.dwell_s = (uint16_t)dwell; rq.settle_s = (uint16_t)settle;
        rq.soc_min = (uint8_t)smin;   rq.soc_max = (uint8_t)smax;
        eff_sweep_start(&rq, err, sizeof(err));
    }
    if (err[0]) {
        char e[160];
        snprintf(e, sizeof(e), "{\"ok\":false,\"err\":\"%s\"}", err);
        return httpd_resp_sendstr(req, e);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------------- standard section order of this page ----------------
 * GET  /api/ui/order        -> {"order":"live,curve,..."}  ("" = built-in)
 * POST /api/ui/order?o=...  -> save it for every browser that has not
 *                              sorted on its own; o= empty erases it.
 * The commas may arrive as %2C: httpd_query_key_value() does not decode. */
static esp_err_t order_get_handler(httpd_req_t *req)
{
    char o[NVS_SEC_ORDER_MAX], buf[NVS_SEC_ORDER_MAX + 16];
    nvs_store_get_sec_order(o, sizeof(o));
    snprintf(buf, sizeof(buf), "{\"order\":\"%s\"}", o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t order_post_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    char q[400], raw[3 * NVS_SEC_ORDER_MAX], o[NVS_SEC_ORDER_MAX];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "o", raw, sizeof(raw)) != ESP_OK)
        raw[0] = '\0';
    size_t n = 0;
    for (const char *p = raw; *p && n + 1 < sizeof(o); p++) {
        if (p[0] == '%' && p[1] == '2' && (p[2] == 'C' || p[2] == 'c')) { o[n++] = ','; p += 2; }
        else o[n++] = *p;
    }
    o[n] = '\0';
    if (nvs_store_set_sec_order(o) != ESP_OK)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"Reihenfolge ung\u00fcltig oder nicht gespeichert\"}");
    ESP_LOGI(TAG, "standard section order: %s", o[0] ? o : "(built-in)");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------------- energy manager settings (Energie tab) ----------------
 * GET  /api/em/cfg -> everything but the Tibber token, which only reports
 *                     whether it is set ("tok":1).
 * POST /api/em/cfg    JSON body, same field names; "token" is optional:
 *                     absent = keep, "" = delete, else store. */
static esp_err_t em_cfg_route_get(httpd_req_t *req)
{
    em_cfg_t c;
    em_cfg_get(&c);
    char buf[1200];
    int o = snprintf(buf, sizeof(buf),
        "{\"lat\":%.5f,\"lon\":%.5f,\"deye_kwh\":%.2f,\"byd_kwh\":%.2f,\"byd_res\":%u,\"byd_max\":%u,"
        "\"charge_kw\":%.2f,\"spread\":%.2f,\"cycle\":%.2f,\"soc_min\":%u,\"soc_max\":%u,"
        "\"mode\":%u,\"byd_ctrl\":%u,\"tok\":%d,\"max\":%d,\"planes\":[",
        c.lat, c.lon, c.deye_kwh, c.byd_kwh, c.byd_reserve_pct, c.byd_max_soc ? c.byd_max_soc : 100,
        c.charge_kw, c.min_spread_ct, c.cycle_cost_ct, c.soc_min_pct, c.soc_max_pct,
        c.mode, c.byd_ctrl, c.tibber_token[0] ? 1 : 0, EM_MAX_PLANES);
    for (int i = 0; i < c.nplanes && o < (int)sizeof(buf) - 120; i++) {
        const em_plane_t *p = &c.plane[i];
        o += snprintf(buf + o, sizeof(buf) - o,
                      "%s{\"name\":\"%s\",\"kwp\":%.2f,\"tilt\":%d,\"az\":%d,\"meter\":%d,\"ac\":%.2f}",
                      i ? "," : "", p->name, p->kwp, p->tilt, p->azimuth, p->meter, p->ac_w / 1000.0);
    }
    snprintf(buf + o, sizeof(buf) - o, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static float jnum(const cJSON *o, const char *k, float def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static esp_err_t em_cfg_route_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char body[1536];
    if (req->content_len <= 0 || req->content_len >= sizeof(body))
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"Anfrage leer oder zu groß\"}");
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"Empfang abgebrochen\"}");
        got += r;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"kein gültiges JSON\"}");
    }
    em_cfg_t c;
    em_cfg_get(&c);
    c.lat             = jnum(root, "lat", c.lat);
    c.lon             = jnum(root, "lon", c.lon);
    c.deye_kwh        = jnum(root, "deye_kwh", c.deye_kwh);
    c.byd_kwh         = jnum(root, "byd_kwh", c.byd_kwh);
    float res         = jnum(root, "byd_res", c.byd_reserve_pct);
    c.byd_reserve_pct = (res >= 0 && res <= 100) ? (uint8_t)(res + 0.5f) : 255;
    float bmax        = jnum(root, "byd_max", c.byd_max_soc ? c.byd_max_soc : 100);
    c.byd_max_soc     = (bmax >= 0 && bmax <= 100) ? (uint8_t)(bmax + 0.5f) : 1;   /* 1: refused by em_cfg */
    c.charge_kw       = jnum(root, "charge_kw", c.charge_kw);
    c.min_spread_ct   = jnum(root, "spread", c.min_spread_ct);
    c.cycle_cost_ct   = jnum(root, "cycle", c.cycle_cost_ct);
    float smin = jnum(root, "soc_min", c.soc_min_pct), smax = jnum(root, "soc_max", c.soc_max_pct);
    float md   = jnum(root, "mode", c.mode), bc = jnum(root, "byd_ctrl", c.byd_ctrl);
    c.soc_min_pct = (smin >= 0 && smin <= 100) ? (uint8_t)(smin + 0.5f) : 255;
    c.soc_max_pct = (smax >= 0 && smax <= 100) ? (uint8_t)(smax + 0.5f) : 255;
    c.mode        = (md >= 0 && md <= 9) ? (uint8_t)md : 255;
    c.byd_ctrl    = bc ? 1 : 0;

    char err[120] = "";
    const cJSON *pl = cJSON_GetObjectItem(root, "planes");
    if (cJSON_IsArray(pl)) {
        int n = cJSON_GetArraySize(pl);
        if (n > EM_MAX_PLANES) snprintf(err, sizeof(err), "höchstens %d PV-Felder", EM_MAX_PLANES);
        else {
            c.nplanes = (uint8_t)n;
            for (int i = 0; i < n; i++) {
                const cJSON *p = cJSON_GetArrayItem(pl, i);
                em_plane_t *d = &c.plane[i];
                memset(d, 0, sizeof(*d));
                const cJSON *nm = cJSON_GetObjectItem(p, "name");
                if (cJSON_IsString(nm)) snprintf(d->name, sizeof(d->name), "%s", nm->valuestring);
                d->kwp     = jnum(p, "kwp", 0.0f);
                float t    = jnum(p, "tilt", -1.0f), a = jnum(p, "az", -1.0f), m = jnum(p, "meter", -1.0f);
                d->tilt    = (t >= -1 && t <= 1000) ? (int16_t)lroundf(t) : -1;
                d->azimuth = (a >= -1 && a <= 1000) ? (int16_t)lroundf(a) : -1;
                d->meter   = (m >= -1 && m <= 127) ? (int8_t)lroundf(m) : -2;
                float ac   = jnum(p, "ac", 0.0f);          /* kW, missing = no limit */
                d->ac_w    = (ac >= 0.0f && ac <= 65.0f) ? (uint16_t)lroundf(ac * 1000.0f) : 65535;
            }
        }
    }
    if (!err[0]) em_cfg_set(&c, err, sizeof(err));
    const cJSON *tok = cJSON_GetObjectItem(root, "token");
    if (!err[0] && cJSON_IsString(tok)) em_cfg_set_token(tok->valuestring, err, sizeof(err));
    cJSON_Delete(root);

    if (err[0]) {
        char e[200];
        snprintf(e, sizeof(e), "{\"ok\":false,\"err\":\"%s\"}", err);
        return httpd_resp_sendstr(req, e);
    }
    pv_forecast_request();            /* planes or location may have changed */
    em_mgr_request("plan");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------- the energy manager's data: /api/em/<what> ----------
 * One GET and one POST handler dispatch on the path (wildcard match), so the
 * whole family costs two of the server's handler slots. The documents are
 * large (192 slots per series), so they are built in one PSRAM buffer; the
 * server runs one handler at a time, so one buffer is enough. */
#define EM_JSON_CAP (40 * 1024)
static char *s_emj;

/* Series as a JSON array, null for unknown, from slot offset `from`. */
static int em_arr(char *b, int o, const char *key, const em_series_t *s, int dec)
{
    o = jcat(b, EM_JSON_CAP, o, "\"%s\":[", key);
    for (int i = 0; i < s->n; i++) {
        float v = s->v[i];
        o = isnan(v) ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                     : jcat(b, EM_JSON_CAP, o, "%s%.*f", i ? "," : "", dec, v);
    }
    return jcat(b, EM_JSON_CAP, o, "]");
}

static int em_num(char *b, int o, const char *key, float v, int dec)
{
    return isnan(v) ? jcat(b, EM_JSON_CAP, o, "\"%s\":null", key)
                    : jcat(b, EM_JSON_CAP, o, "\"%s\":%.*f", key, dec, v);
}

/* Strings that come from feeds (error texts) may hold quotes. */
static int em_str(char *b, int o, const char *key, const char *s)
{
    o = jcat(b, EM_JSON_CAP, o, "\"%s\":\"", key);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') o = jcat(b, EM_JSON_CAP, o, "\\%c", *s);
        else if ((unsigned char)*s >= 0x20) o = jcat(b, EM_JSON_CAP, o, "%c", *s);
    }
    return jcat(b, EM_JSON_CAP, o, "\"");
}

static int em_prices(char *b)
{
    static EXT_RAM_BSS_ATTR price_set_t ps;
    static EXT_RAM_BSS_ATTR grid_mix_t mx;
    price_feed_get(&ps);
    price_feed_get_mix(&mx);
    int o = jcat(b, EM_JSON_CAP, 0, "{");
    o = em_str(b, o, "src", ps.src);
    o = jcat(b, EM_JSON_CAP, o, ",\"fetched\":%lld,", (long long)ps.fetched);
    o = em_str(b, o, "err", ps.err);
    o = jcat(b, EM_JSON_CAP, o, ",\"tomorrow\":%d,\"t0\":%lld,\"step\":%d,",
             ps.tomorrow ? 1 : 0, (long long)ps.total.t0, EM_SLOT_S);
    o = em_arr(b, o, "total", &ps.total, 2);
    o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_arr(b, o, "energy", &ps.energy, 2);
    o = jcat(b, EM_JSON_CAP, o, ",\"mix\":{\"t0\":%lld,\"step\":%d,\"fetched\":%lld,", (long long)mx.ren.t0, EM_SLOT_S, (long long)mx.fetched);
    o = em_str(b, o, "err", mx.err);
    o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_arr(b, o, "ren", &mx.ren, 1);      o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_arr(b, o, "solar", &mx.solar, 1);  o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_arr(b, o, "wind_on", &mx.wind_on, 1); o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_arr(b, o, "wind_off", &mx.wind_off, 1);
    /* Generation in GW on the same grid as the shares (both start at local
     * midnight). Wind = onshore + offshore; a missing offshore value counts
     * as 0 (it is the small part), a missing onshore one as unknown. */
    static EXT_RAM_BSS_ATTR em_series_t wind;
    wind = mx.wind_on_gw;
    for (int i = 0; i < wind.n; i++) {
        float off = em_series_at(&mx.wind_off_gw, wind.t0 + (int64_t)i * EM_SLOT_S);
        if (!isnan(wind.v[i]) && !isnan(off)) wind.v[i] += off;
    }
    o = jcat(b, EM_JSON_CAP, o, ",\"gen_t0\":%lld,\"gen_fetched\":%lld,", (long long)wind.t0, (long long)mx.gen_fetched);
    o = em_arr(b, o, "wind_gw", &wind, 2);    o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_arr(b, o, "solar_gw", &mx.solar_gw, 2);
    o = jcat(b, EM_JSON_CAP, o, "}");

    /* Is each source reachable: last attempt, last success, next attempt. */
    static const char *const SRC_NAME[PF_SRC_N] = { "Tibber", "Energy-Charts Preise", "Energy-Charts Strommix",
                                                     "Energy-Charts Erzeugung" };
    static const char *const SRC_HOST[PF_SRC_N] = { "api.tibber.com", "api.energy-charts.info",
                                                     "api.energy-charts.info", "api.energy-charts.info" };
    static EXT_RAM_BSS_ATTR pf_src_t src[PF_SRC_N];
    static EXT_RAM_BSS_ATTR pv_fc_t pv;
    int64_t now = time(NULL);
    price_feed_sources(now, src);
    pv_forecast_get(&pv);
    o = jcat(b, EM_JSON_CAP, o, ",\"now\":%lld,\"sources\":[", (long long)now);
    for (int i = 0; i < PF_SRC_N; i++) {
        const pf_src_t *q = &src[i];
        o = jcat(b, EM_JSON_CAP, o, "%s{\"name\":\"%s\",\"host\":\"%s\",\"used\":%d,\"last_try\":%lld,"
                 "\"last_ok\":%lld,\"next\":%lld,\"fails\":%u,\"r429\":%d,\"tomorrow\":%d,\"stage\":%u,",
                 i ? "," : "", SRC_NAME[i], SRC_HOST[i], q->used ? 1 : 0, (long long)q->last_try,
                 (long long)q->last_ok, (long long)q->next, q->fails, q->r429 ? 1 : 0, q->tomorrow ? 1 : 0, q->stage);
        o = em_str(b, o, "err", q->err);
        o = jcat(b, EM_JSON_CAP, o, "}");
    }
    /* Open-Meteo (weather for the PV forecast) keeps only its last success. */
    o = jcat(b, EM_JSON_CAP, o, ",{\"name\":\"Open-Meteo Wetter\",\"host\":\"api.open-meteo.com\",\"used\":1,"
             "\"last_ok\":%lld,\"fails\":%d,", (long long)pv.fetched, pv.err[0] ? 1 : 0);
    o = em_str(b, o, "err", pv.err);
    return jcat(b, EM_JSON_CAP, o, "}]}");
}

static int em_forecast(char *b)
{
    static EXT_RAM_BSS_ATTR pv_fc_t pv;
    pv_forecast_get(&pv);
    int o = jcat(b, EM_JSON_CAP, 0, "{\"fetched\":%lld,", (long long)pv.fetched);
    o = em_str(b, o, "err", pv.err);
    o = jcat(b, EM_JSON_CAP, o, ",\"t0\":%lld,\"step\":%d,\"meas_t0\":%lld,",
             (long long)pv.total_kw.t0, EM_SLOT_S, (long long)pv.meas_t0);
    o = em_arr(b, o, "total", &pv.total_kw, 3);
    o = jcat(b, EM_JSON_CAP, o, ",\"planes\":[");
    for (int i = 0; i < pv.n; i++) {
        const pv_plane_fc_t *p = &pv.p[i];
        o = jcat(b, EM_JSON_CAP, o, "%s{", i ? "," : "");
        o = em_str(b, o, "name", p->name);
        o = jcat(b, EM_JSON_CAP, o, ",");
        o = em_num(b, o, "cal", p->cal, 3);              o = jcat(b, EM_JSON_CAP, o, ",");
        o = em_num(b, o, "kwh_today", p->kwh_today, 2);  o = jcat(b, EM_JSON_CAP, o, ",");
        o = em_num(b, o, "kwh_tomorrow", p->kwh_tomorrow, 2); o = jcat(b, EM_JSON_CAP, o, ",");
        o = em_num(b, o, "meas_kwh_today", p->meas_kwh_today, 2); o = jcat(b, EM_JSON_CAP, o, ",");
        o = em_arr(b, o, "kw", &p->kw, 3);
        /* measured today, per quarter hour from meas_t0: kWh and coverage % */
        int mn = pv.meas_t0 ? pv.meas_n : 0;
        o = jcat(b, EM_JSON_CAP, o, ",\"mkwh\":[");
        for (int k = 0; k < mn; k++)
            o = isnan(p->meas_kwh[k]) ? jcat(b, EM_JSON_CAP, o, "%snull", k ? "," : "")
                                      : jcat(b, EM_JSON_CAP, o, "%s%.4f", k ? "," : "", p->meas_kwh[k]);
        o = jcat(b, EM_JSON_CAP, o, "],\"mcov\":[");
        for (int k = 0; k < mn; k++) o = jcat(b, EM_JSON_CAP, o, "%s%u", k ? "," : "", p->meas_cov[k]);
        o = jcat(b, EM_JSON_CAP, o, "]}");
    }
    return jcat(b, EM_JSON_CAP, o, "]}");
}

static int em_load(char *b)
{
    static EXT_RAM_BSS_ATTR load_profile_t lp;
    load_profile_get(&lp);
    int o = jcat(b, EM_JSON_CAP, 0, "{\"days_wd\":%u,\"days_we\":%u,\"wd\":[", lp.days_wd, lp.days_we);
    for (int i = 0; i < 96; i++)
        o = isnan(lp.wd_kw[i]) ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                               : jcat(b, EM_JSON_CAP, o, "%s%.3f", i ? "," : "", lp.wd_kw[i]);
    o = jcat(b, EM_JSON_CAP, o, "],\"we\":[");
    for (int i = 0; i < 96; i++)
        o = isnan(lp.we_kw[i]) ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                               : jcat(b, EM_JSON_CAP, o, "%s%.3f", i ? "," : "", lp.we_kw[i]);
    /* The long-term record: where the profiles come from, how far it goes
     * back, and today so far. */
    char from[16] = "";
    if (lp.hist_oldest) {
        int y, m, d;
        load_hist_date(lp.hist_oldest, &y, &m, &d);
        snprintf(from, sizeof(from), "%04d-%02d-%02d", y % 10000, m % 100, d % 100);
    }
    o = jcat(b, EM_JSON_CAP, o, "],\"src\":[%u,%u],\"avg_days\":[%u,%u],\"hist_ok\":%d,\"hist_days\":%u,"
             "\"hist_from\":\"%s\",\"today\":[",
             lp.src[0], lp.src[1], lp.avg_days[0], lp.avg_days[1], lp.hist_ok ? 1 : 0, lp.hist_days, from);
    for (int i = 0; i < 96; i++)
        o = isnan(lp.today_kw[i]) ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                                  : jcat(b, EM_JSON_CAP, o, "%s%.3f", i ? "," : "", lp.today_kw[i]);
    return jcat(b, EM_JSON_CAP, o, "]}");
}

/* GET /api/em/loadhist?off=0&n=14: stored days, newest first -- date,
 * weekday (0 = Sunday), energy, and the 96 quarter-hour means in W. At most
 * 31 days per call (~17 kB); `total` tells how many there are. */
static int em_loadhist(httpd_req_t *req, char *b)
{
    char q[48] = "", v[12];
    int off = 0, n = 14;
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "off", v, sizeof(v)) == ESP_OK) off = atoi(v);
    if (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) n = atoi(v);
    if (off < 0) off = 0;
    if (n < 1) n = 1;
    if (n > 31) n = 31;
    int32_t oldest = 0;
    int total = load_hist_count(&oldest);
    int o = jcat(b, EM_JSON_CAP, 0, "{\"total\":%d,\"off\":%d,\"days\":[", total, off);
    static EXT_RAM_BSS_ATTR lh_day_t d;
    for (int k = 0; k < n && load_hist_get(off + k, &d); k++) {
        int y, m, dd;
        load_hist_date(d.day, &y, &m, &dd);
        double kwh = 0.0;
        for (int i = 0; i < LH_SLOTS; i++) if (d.w[i] != LH_NONE) kwh += d.w[i] / 4000.0;
        o = jcat(b, EM_JSON_CAP, o, "%s{\"date\":\"%04d-%02d-%02d\",\"wd\":%u,\"kwh\":%.2f,\"w\":[",
                 k ? "," : "", y % 10000, m % 100, dd % 100, d.wday, kwh);
        for (int i = 0; i < LH_SLOTS; i++)
            o = d.w[i] == LH_NONE ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                                  : jcat(b, EM_JSON_CAP, o, "%s%u", i ? "," : "", d.w[i]);
        o = jcat(b, EM_JSON_CAP, o, "]}");
    }
    return jcat(b, EM_JSON_CAP, o, "]}");
}

/* GET /api/em/hist?from=<unix>&n=<slots>: what happened per quarter hour
 * (em_hist.h) from `from` on, at most EM_MAX_SLOTS. Without `from`: since
 * local midnight. null where nothing was recorded. */
static int em_hist_json(httpd_req_t *req, char *b)
{
    char q[64] = "", v[24];
    int64_t now = time(NULL), from = 0;
    int n = 0;
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (httpd_query_key_value(q, "from", v, sizeof(v)) == ESP_OK) from = atoll(v);
    if (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) n = atoi(v);
    if (from <= 0) {
        time_t tt = (time_t)now;
        struct tm tm;
        localtime_r(&tt, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        tm.tm_isdst = -1;
        from = (int64_t)mktime(&tm);
    }
    from = em_slot_floor(from);
    if (n <= 0) n = (int)((now - from) / EM_SLOT_S) + 1;
    if (n > EM_MAX_SLOTS) n = EM_MAX_SLOTS;
    if (n < 1) n = 1;
    static EXT_RAM_BSS_ATTR eh_slot_t h[EM_MAX_SLOTS];
    int found = em_hist_range(from, n, h);
    int64_t oldest = 0;
    int total = em_hist_count(&oldest);
    int o = jcat(b, EM_JSON_CAP, 0, "{\"t0\":%lld,\"step\":%d,\"n\":%d,\"found\":%d,\"total\":%d,\"oldest\":%lld",
                 (long long)from, EM_SLOT_S, n, found, total, (long long)oldest);
    o = jcat(b, EM_JSON_CAP, o, ",\"act\":[");
    for (int i = 0; i < n; i++)
        o = h[i].valid && h[i].act != EH_ACT_NONE ? jcat(b, EM_JSON_CAP, o, "%s%u", i ? "," : "", h[i].act)
                                                  : jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "");
    o = jcat(b, EM_JSON_CAP, o, "],\"flags\":[");
    for (int i = 0; i < n; i++)
        o = h[i].valid ? jcat(b, EM_JSON_CAP, o, "%s%u", i ? "," : "", h[i].flags | (h[i].mode << 4))
                       : jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "");
    /* flags: bit 0 acting, bit 1 paused, bits 4.. EM_MODE_* */
    static const struct { const char *key; size_t off; int dec; } F[] = {
        { "soc",   offsetof(eh_slot_t, deye_soc),   1 },
        { "byd",   offsetof(eh_slot_t, byd_soc),    1 },
        { "bat",   offsetof(eh_slot_t, deye_bat_w), 0 },
        { "grid",  offsetof(eh_slot_t, grid_w),     0 },
        { "pv",    offsetof(eh_slot_t, pv_w),       0 },
        { "house", offsetof(eh_slot_t, house_w),    0 },
        { "price", offsetof(eh_slot_t, price_ct),   2 },
    };
    for (size_t f = 0; f < sizeof(F) / sizeof(F[0]); f++) {
        o = jcat(b, EM_JSON_CAP, o, "],\"%s\":[", F[f].key);
        for (int i = 0; i < n; i++) {
            float x = *(const float *)((const char *)&h[i] + F[f].off);
            o = !h[i].valid || isnan(x) ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                                        : jcat(b, EM_JSON_CAP, o, "%s%.*f", i ? "," : "", F[f].dec, x);
        }
    }
    return jcat(b, EM_JSON_CAP, o, "]}");
}

static int em_plan_json(char *b)
{
    static EXT_RAM_BSS_ATTR em_plan_t p;
    static EXT_RAM_BSS_ATTR em_status_t st;
    static EXT_RAM_BSS_ATTR fronius_storage_status_t fs;
    em_mgr_get_plan(&p);
    em_mgr_get_status(&st);
    fronius_storage_get(&fs);
    int o = jcat(b, EM_JSON_CAP, 0, "{\"mode\":%u,\"updated\":%lld,\"acting\":%d,\"paused\":%d,",
                 st.mode, (long long)st.updated, st.acting ? 1 : 0, st.paused ? 1 : 0);
    o = em_str(b, o, "err", st.err);
    o = jcat(b, EM_JSON_CAP, o, ",\"t0\":%lld,\"step\":%d,\"n\":%u,\"act\":[", (long long)p.t0, EM_SLOT_S, p.n);
    for (int i = 0; i < p.n; i++) o = jcat(b, EM_JSON_CAP, o, "%s%u", i ? "," : "", p.s[i].act);
    o = jcat(b, EM_JSON_CAP, o, "],\"kw\":[");
    for (int i = 0; i < p.n; i++) o = jcat(b, EM_JSON_CAP, o, "%s%.2f", i ? "," : "", p.s[i].charge_kw);
    o = jcat(b, EM_JSON_CAP, o, "],\"soc\":[");
    for (int i = 0; i < p.n; i++) o = jcat(b, EM_JSON_CAP, o, "%s%.1f", i ? "," : "", p.s[i].soc_end);
    o = jcat(b, EM_JSON_CAP, o, "],\"grid\":[");
    for (int i = 0; i < p.n; i++) o = jcat(b, EM_JSON_CAP, o, "%s%.2f", i ? "," : "", p.s[i].grid_kw);
    o = jcat(b, EM_JSON_CAP, o, "],\"for\":[");
    for (int i = 0; i < p.n; i++) o = jcat(b, EM_JSON_CAP, o, "%s%d", i ? "," : "", p.s[i].for_slot);
    o = jcat(b, EM_JSON_CAP, o, "],\"cost_plan\":%.1f,\"cost_base\":%.1f,\"now\":{\"act\":%u,",
             p.n ? p.cost_plan_ct : 0.0f, p.n ? p.cost_base_ct : 0.0f, st.act_now);
    o = em_str(b, o, "why", st.why);
    o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "price", st.price_now_ct, 2);  o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "ren", st.ren_now_pct, 1);     o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "solar", st.solar_now_pct, 1); o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "wind", st.wind_now_pct, 1);
    o = jcat(b, EM_JSON_CAP, o, "},");
    o = em_num(b, o, "saving", st.saving_ct, 1);          o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "pv_today", st.pv_today_kwh, 2);     o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "pv_tomorrow", st.pv_tomorrow_kwh, 2); o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "deye_soc", st.deye_soc, 1);
    o = jcat(b, EM_JSON_CAP, o, ",\"next_charge\":[%lld,%lld],\"price\":[", (long long)st.next_charge_from, (long long)st.next_charge_to);
    /* The prices the plan used, for the page's bar chart. */
    static EXT_RAM_BSS_ATTR price_set_t ps;
    price_feed_get(&ps);
    for (int i = 0; i < p.n; i++) {
        float v = em_series_at(&ps.total, p.t0 + (int64_t)i * EM_SLOT_S);
        o = isnan(v) ? jcat(b, EM_JSON_CAP, o, "%snull", i ? "," : "")
                     : jcat(b, EM_JSON_CAP, o, "%s%.2f", i ? "," : "", v);
    }
    modbus_tcp_status_t ts;
    modbus_tcp_get_status(&ts);
    bat_lock_t ld, lb;
    bat_lock_get(BAT_DEYE, &ld);
    bat_lock_get(BAT_BYD, &lb);
    em_cfg_t lcfg;
    em_cfg_get(&lcfg);
    o = jcat(b, EM_JSON_CAP, o, "],\"lock\":{\"deye\":{\"on\":%d,\"until\":%lld,\"held\":%d},"
             "\"byd\":{\"on\":%d,\"until\":%lld,\"held\":%d,\"allowed\":%d}}",
             ld.on ? 1 : 0, (long long)ld.until, modbus_rtu_lock_active() ? 1 : 0,
             lb.on ? 1 : 0, (long long)lb.until, fs.blocked && fs.lock_req ? 1 : 0, lcfg.byd_ctrl ? 1 : 0);
    deye_force_t dfo;
    deye_force_get(&dfo);
    o = jcat(b, EM_JSON_CAP, o, ",\"deye_force\":{\"mode\":%u,\"w\":%u,\"until\":%lld,\"soc\":%u,\"active\":%d,",
             dfo.mode, dfo.w, (long long)dfo.until, dfo.soc, dfo.active ? 1 : 0);
    o = em_str(b, o, "end", dfo.end);
    o = jcat(b, EM_JSON_CAP, o, "}");
    o = jcat(b, EM_JSON_CAP, o, ",\"byd\":{\"ok\":%d,", fs.ok ? 1 : 0);
    o = em_num(b, o, "soc", fs.ok ? fs.soc_pct : (ts.byd_soc > 0.0f ? ts.byd_soc : NAN), 1);
    o = jcat(b, EM_JSON_CAP, o, ",\"blocked\":%d,\"no_revert\":%d,\"forcing\":%d,\"force\":%u,"
             "\"force_w\":%u,\"force_until\":%lld,\"force_soc\":%.0f,",
             fs.blocked ? 1 : 0, fs.no_revert ? 1 : 0, fs.forcing ? 1 : 0, fs.force,
             fs.force_w, (long long)fs.force_until, fs.force ? (double)fs.force_soc : 0.0);
    o = jcat(b, EM_JSON_CAP, o, "\"cap\":%u,\"cap_on\":%d,",
             fs.cap_pct ? fs.cap_pct : 100, fs.cap_on ? 1 : 0);
    o = em_str(b, o, "force_end", fs.force_end);
    o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_str(b, o, "err", fs.err);
    return jcat(b, EM_JSON_CAP, o, "}}");
}

static int em_fronius(char *b)
{
    static EXT_RAM_BSS_ATTR fronius_storage_status_t fs;
    fronius_storage_get(&fs);
    int o = jcat(b, EM_JSON_CAP, 0, "{\"ok\":%d,", fs.ok ? 1 : 0);
    o = em_str(b, o, "ip", fs.ip);
    o = jcat(b, EM_JSON_CAP, o, ",\"addr\":%u,", fs.addr);
    o = em_num(b, o, "soc", fs.soc_pct, 2);         o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "reserve", fs.reserve_pct, 2); o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "out_rte", fs.out_rte_pct, 2); o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "in_rte", fs.in_rte_pct, 2);   o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_num(b, o, "wchamax", fs.wchamax_w, 0);
    o = jcat(b, EM_JSON_CAP, o, ",\"ctl_mod\":%u,\"rvrt\":%u,\"cha_st\":%u,\"grid_charge\":%u,\"blocked\":%d,\"read_at\":%lld,"
             "\"no_revert\":%d,\"owed\":%d,",
             fs.ctl_mod, fs.rvrt_s, fs.cha_st, fs.grid_charge, fs.blocked ? 1 : 0, (long long)fs.read_at,
             fs.no_revert ? 1 : 0, fs.owed ? 1 : 0);
    o = jcat(b, EM_JSON_CAP, o, "\"forcing\":%d,\"force\":%u,\"force_w\":%u,\"force_until\":%lld,",
             fs.forcing ? 1 : 0, fs.force, fs.force_w, (long long)fs.force_until);
    o = em_num(b, o, "force_soc", fs.force ? fs.force_soc : NAN, 0); o = jcat(b, EM_JSON_CAP, o, ",");
    /* the operator's base values while a setting of ours is in force (%) */
    o = jcat(b, EM_JSON_CAP, o, "\"base_known\":%d,\"cap\":%u,\"cap_on\":%d,\"lock_req\":%d,",
             fs.base_known ? 1 : 0, fs.cap_pct ? fs.cap_pct : 100, fs.cap_on ? 1 : 0, fs.lock_req ? 1 : 0);
    if (fs.base_known) {
        o = em_num(b, o, "base_out", (int16_t)fs.base_out / 100.0f, 2); o = jcat(b, EM_JSON_CAP, o, ",");
        o = em_num(b, o, "base_in", fs.base_in == 0xFFFF ? NAN : (int16_t)fs.base_in / 100.0f, 2);
        o = jcat(b, EM_JSON_CAP, o, ",");
    }
    o = em_str(b, o, "force_end", fs.force_end);                   o = jcat(b, EM_JSON_CAP, o, ",");
    o = em_str(b, o, "err", fs.err);
    return jcat(b, EM_JSON_CAP, o, "}");
}

static esp_err_t em_get_handler(httpd_req_t *req)
{
    const char *what = req->uri + strlen("/api/em/");
    size_t wl = strcspn(what, "?");
    if (wl == 3 && !strncmp(what, "cfg", 3)) return em_cfg_route_get(req);
    if (!s_emj) s_emj = heap_caps_malloc(EM_JSON_CAP, MALLOC_CAP_SPIRAM);
    if (!s_emj) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    int n = -1;
    if      (wl == 6 && !strncmp(what, "prices", 6))   n = em_prices(s_emj);
    else if (wl == 8 && !strncmp(what, "forecast", 8)) n = em_forecast(s_emj);
    else if (wl == 4 && !strncmp(what, "load", 4))     n = em_load(s_emj);
    else if (wl == 8 && !strncmp(what, "loadhist", 8)) n = em_loadhist(req, s_emj);
    else if (wl == 4 && !strncmp(what, "plan", 4))     n = em_plan_json(s_emj);
    else if (wl == 4 && !strncmp(what, "hist", 4))     n = em_hist_json(req, s_emj);
    else if (wl == 7 && !strncmp(what, "fronius", 7)) { em_mgr_want_fronius(); n = em_fronius(s_emj); }
    if (n < 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown /api/em route");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_emj, n);
}

static esp_err_t em_post_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    const char *what = req->uri + strlen("/api/em/");
    size_t wl = strcspn(what, "?");
    if (wl == 3 && !strncmp(what, "cfg", 3)) return em_cfg_route_post(req);
    httpd_resp_set_type(req, "application/json");
    char q[160] = "", v[16] = "", err[120] = "", okmsg[48] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    if (wl == 4 && !strncmp(what, "mode", 4)) {
        if (httpd_query_key_value(q, "m", v, sizeof(v)) != ESP_OK || v[0] < '0' || v[0] > '2' || v[1])
            snprintf(err, sizeof(err), "m=0 (aus), 1 (nur planen) oder 2 (Automatik)");
        else em_mgr_set_mode((uint8_t)(v[0] - '0'), err, sizeof(err));
    } else if (wl == 4 && !strncmp(what, "deye", 4)) {
        /* The Deye's forced mode: cmd=charge|discharge&w=…&min=…&soc=…, cmd=stop. */
        char cmd[12] = "";
        httpd_query_key_value(q, "cmd", cmd, sizeof(cmd));
        if (!strcmp(cmd, "stop")) {
            deye_force_set(DEYE_FORCE_OFF, 0, 0, 0, 0);
        } else if (!strcmp(cmd, "charge") || !strcmp(cmd, "discharge")) {
            bool chg = cmd[0] == 'c';
            long w  = httpd_query_key_value(q, "w", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : -1;
            long mi = httpd_query_key_value(q, "min", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : 0;
            long sc = httpd_query_key_value(q, "soc", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : (chg ? 100 : 20);
            esp_err_t e = (w < 0 || w > 65535 || mi < 0 || mi > 65535 || sc < 0 || sc > 255) ? ESP_ERR_INVALID_ARG
                        : deye_force_set(chg ? DEYE_FORCE_CHARGE : DEYE_FORCE_DISCHARGE,
                                         (uint16_t)w, (uint16_t)mi, (uint8_t)sc, (int64_t)time(NULL));
            if (e == ESP_ERR_INVALID_ARG)
                snprintf(err, sizeof(err), "w=%d…%d, min=1…%d, soc=10…100", DEYE_FORCE_MIN_W, DEYE_POWER_MAX, DEYE_FORCE_MAX_MIN);
            else if (e == ESP_ERR_INVALID_STATE) snprintf(err, sizeof(err), "Uhrzeit noch nicht gestellt");
            else if (e != ESP_OK)                snprintf(err, sizeof(err), "Deye hat nicht übernommen");
        } else {
            snprintf(err, sizeof(err), "cmd=charge|discharge&w=…&min=…&soc=… oder cmd=stop");
        }
    } else if (wl == 4 && !strncmp(what, "lock", 4)) {
        /* The operator's battery locks: bat=deye|byd (missing: deye), on=1
         * (min=0 or missing: until lifted, else 1..1440 minutes), on=0 lifts. */
        char bt[8] = "deye";
        httpd_query_key_value(q, "bat", bt, sizeof(bt));
        bool byd = !strcmp(bt, "byd");
        bool on = httpd_query_key_value(q, "on", v, sizeof(v)) == ESP_OK && v[0] == '1';
        long mi = httpd_query_key_value(q, "min", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : 0;
        em_cfg_t c;
        em_cfg_get(&c);
        esp_err_t e = (!byd && strcmp(bt, "deye")) || mi < 0 ? ESP_ERR_INVALID_ARG
                    : (byd && on && !c.byd_ctrl) ? ESP_ERR_NOT_ALLOWED
                    : bat_lock_set(byd ? BAT_BYD : BAT_DEYE, on, (uint32_t)mi, (int64_t)time(NULL));
        if (byd) em_mgr_want_fronius();
        if (e == ESP_ERR_INVALID_ARG)        snprintf(err, sizeof(err), "bat=deye|byd, min=0 (bis aufgehoben) oder 1…%d", BAT_LOCK_MAX_MIN);
        else if (e == ESP_ERR_NOT_ALLOWED)   snprintf(err, sizeof(err), "BYD-Steuerung ist in den Einstellungen nicht erlaubt");
        else if (e == ESP_ERR_INVALID_STATE) snprintf(err, sizeof(err), "Uhrzeit noch nicht gestellt");
        else if (e != ESP_OK)                snprintf(err, sizeof(err), "abgelehnt");
    } else if (wl == 7 && !strncmp(what, "refresh", 7)) {
        if (httpd_query_key_value(q, "what", v, sizeof(v)) != ESP_OK) snprintf(v, sizeof(v), "all");
        if (strcmp(v, "prices") && strcmp(v, "forecast") && strcmp(v, "plan") && strcmp(v, "all"))
            snprintf(err, sizeof(err), "what=prices, forecast, plan oder all");
        else em_mgr_request(v);
    } else if (wl == 7 && !strncmp(what, "fronius", 7)) {
        em_mgr_want_fronius();
        em_cfg_t c;
        em_cfg_get(&c);
        char cmd[16] = "";
        httpd_query_key_value(q, "cmd", cmd, sizeof(cmd));
        if (!c.byd_ctrl) {
            snprintf(err, sizeof(err), "BYD-Steuerung ist in den Einstellungen nicht erlaubt");
        } else if (!strcmp(cmd, "block")) {
            /* The operator's BYD lock (bat_lock); min as in /api/em/lock. */
            int on = httpd_query_key_value(q, "on", v, sizeof(v)) == ESP_OK && v[0] == '1';
            long mi = httpd_query_key_value(q, "min", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : 0;
            if (mi < 0 || bat_lock_set(BAT_BYD, on, (uint32_t)mi, (int64_t)time(NULL)) != ESP_OK)
                snprintf(err, sizeof(err), "abgelehnt");
        } else if (!strcmp(cmd, "reserve")) {
            float pct = httpd_query_key_value(q, "pct", v, sizeof(v)) == ESP_OK ? strtof(v, NULL) : -1.0f;
            if (!(pct >= 0.0f && pct <= 100.0f)) snprintf(err, sizeof(err), "pct=0…100");
            else if (fronius_storage_set_reserve(pct) != ESP_OK) snprintf(err, sizeof(err), "Reserve nicht übernommen");
        } else if (!strcmp(cmd, "charge") || !strcmp(cmd, "discharge")) {
            /* Forced operation: w (W), min (minutes, 0/missing = 60), soc (%,
             * charge stops at >=, discharge at <=; default 100 / 20). */
            bool chg = cmd[0] == 'c';
            long w = httpd_query_key_value(q, "w", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : -1;
            long mi = httpd_query_key_value(q, "min", v, sizeof(v)) == ESP_OK ? strtol(v, NULL, 10) : 0;
            float soc = httpd_query_key_value(q, "soc", v, sizeof(v)) == ESP_OK ? strtof(v, NULL)
                                                                                 : (chg ? 100.0f : 20.0f);
            int64_t now = (int64_t)time(NULL);
            if (now < 1700000000LL)
                snprintf(err, sizeof(err), "Uhrzeit noch nicht gestellt");
            else if (w < FRONIUS_FORCE_MIN_W || w > FRONIUS_FORCE_MAX_W)
                snprintf(err, sizeof(err), "w=%d…%d", FRONIUS_FORCE_MIN_W, FRONIUS_FORCE_MAX_W);
            else if (mi < 0 || mi > FRONIUS_FORCE_MAX_MIN)
                snprintf(err, sizeof(err), "min=1…%d", FRONIUS_FORCE_MAX_MIN);
            else if (!(soc >= 0.0f && soc <= 100.0f))
                snprintf(err, sizeof(err), "soc=0…100");
            else if (fronius_storage_force(chg ? FRONIUS_FORCE_CHARGE : FRONIUS_FORCE_DISCHARGE,
                                           (uint16_t)w, (uint16_t)mi, soc, now) != ESP_OK)
                snprintf(err, sizeof(err), "abgelehnt");
        } else if (!strcmp(cmd, "set")) {
            /* The sliders of the Fronius card: reg=reserve|out|in, v in %. */
            char rg[12] = "";
            httpd_query_key_value(q, "reg", rg, sizeof(rg));
            float val = httpd_query_key_value(q, "v", v, sizeof(v)) == ESP_OK ? strtof(v, NULL) : NAN;
            int r = !strcmp(rg, "reserve") ? FRONIUS_REG_RESERVE : !strcmp(rg, "out") ? FRONIUS_REG_OUT
                  : !strcmp(rg, "in") ? FRONIUS_REG_IN : -1;
            bool as_base = false;
            esp_err_t e = r < 0 ? ESP_ERR_INVALID_ARG : fronius_storage_set((fronius_reg_t)r, val, &as_base);
            if (e == ESP_ERR_INVALID_ARG)
                snprintf(err, sizeof(err), "reg=reserve|out|in, v=0…100");
            else if (e != ESP_OK) {
                fronius_storage_status_t fs;
                fronius_storage_get(&fs);
                snprintf(err, sizeof(err), "nicht übernommen%s%.80s", fs.err[0] ? ": " : "", fs.err);
            } else if (as_base) {
                snprintf(okmsg, sizeof(okmsg), "gilt nach Ende der Sperre");
            }
        } else if (!strcmp(cmd, "stop")) {
            fronius_storage_force(FRONIUS_FORCE_OFF, 0, 0, 0, 0);
        } else {
            snprintf(err, sizeof(err), "cmd=block&on=0|1, cmd=charge|discharge&w=…&min=…&soc=…, cmd=stop "
                                       "oder cmd=reserve&pct=…");
        }
    } else {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown /api/em route");
    }
    if (err[0]) {
        char e[200];
        snprintf(e, sizeof(e), "{\"ok\":false,\"err\":\"%s\"}", err);
        return httpd_resp_sendstr(req, e);
    }
    if (okmsg[0]) {
        char e[120];
        snprintf(e, sizeof(e), "{\"ok\":true,\"msg\":\"%s\"}", okmsg);
        return httpd_resp_sendstr(req, e);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------------- the Deye as a Fronius producer meter ----------------
 * Configuration and status for the "Fronius" tab of /deye (fronius_meter.h).
 * The meter lives on the TCP bridge's listener, so its port is the bridge's
 * and is only reported here, not set. */

static int frm_json(char *buf, size_t cap)
{
    mb_rtu_cfg_t c;
    modbus_rtu_get_cfg(&c);
    fronius_meter_status_t f;
    fronius_meter_get_status(&f);
    modbus_gw_status_t g;
    modbus_gw_get_status(&g);
    struct in_addr ip = { .s_addr = f.client_ip };
    char ipbuf[16] = "";
    if (f.client_ip) inet_ntoa_r(ip, ipbuf, sizeof(ipbuf));
    return snprintf(buf, cap,
        "{\"en\":%d,\"unit\":%u,\"sn\":%lu,\"snt\":\"%s\",\"port\":%u,\"gw\":%d,\"listening\":%d,"
        "\"fresh\":%d,\"req\":%u,\"ok\":%u,\"ex\":%u,\"silent\":%u,"
        "\"age\":%u,\"client\":\"%s\",\"p\":[%.0f,%.0f,%.0f],\"total\":%.0f,"
        "\"imp_kwh\":%.3f,\"exp_kwh\":%.3f}",
        c.frm_enabled ? 1 : 0, (unsigned)c.frm_unit, (unsigned long)c.frm_serial,
        c.frm_serial_txt, (unsigned)c.gw_port,
        c.gw_enabled ? 1 : 0, g.running ? 1 : 0, f.fresh ? 1 : 0,
        (unsigned)f.requests, (unsigned)f.answered, (unsigned)f.exceptions,
        (unsigned)f.silent, (unsigned)f.age_ms, ipbuf,
        f.p[0], f.p[1], f.p[2], f.p_total, f.import_kwh, f.export_kwh);
}

/* String query parameter; false if absent. */
static bool qparam_str(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, key, out, len) == ESP_OK;
}

/* GET /api/frm -> configuration + live status. */
static esp_err_t frm_handler(httpd_req_t *req)
{
    char buf[440];
    frm_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

/* GET /api/frm/set?en=0|1&unit=N&sn=<number>&snt=<7 chars>
 *   -> {"ok":true,"st":{...}} or {"ok":false,"err":".."}.
 * Every parameter optional; a missing one keeps its value. */
static esp_err_t frm_set_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    mb_rtu_cfg_t c;
    modbus_rtu_get_cfg(&c);
    int en   = qparam_int(req, "en",   c.frm_enabled);
    int unit = qparam_int(req, "unit", c.frm_unit);

    httpd_resp_set_type(req, "application/json");
    const char *err = NULL;
    char v[24];
    if (qparam_str(req, "sn", v, sizeof(v))) {
        char *end = NULL;
        unsigned long long sn = strtoull(v, &end, 10);
        if (!v[0] || *end || sn < 1 || sn > 0xFFFFFFFFull) err = "Seriennummer: Zahl von 1 bis 4294967295";
        else c.frm_serial = (uint32_t)sn;
    }
    if (!err && qparam_str(req, "snt", v, sizeof(v))) {
        if (!frm_serial_txt_ok(v)) {
            err = "Seriennummer-Text: genau 7 Buchstaben oder Ziffern";
        } else {
            memset(c.frm_serial_txt, 0, sizeof(c.frm_serial_txt));
            memcpy(c.frm_serial_txt, v, FRM_SERIAL_TXT_LEN);
        }
    }
    if (err) {
        /* reported below */
    } else if (unit < 1 || unit > 247) {
        err = "Unit-ID 1 bis 247";
    } else {
        /* A unit id that a bridged bus answers under would take that device
         * off the bridge -- most likely the Deye itself. */
        for (int i = 0; i < MB_RTU_BUSES && c.gw_enabled; i++) {
            if (modbus_rtu_bus_can_gateway(i) && c.bus[i].slave_id == unit) {
                err = "Unit-ID ist die Slave-ID eines gebr\u00fcckten RS485-Busses";
                break;
            }
        }
    }
    if (err) {
        char e[120];
        snprintf(e, sizeof(e), "{\"ok\":false,\"err\":\"%s\"}", err);
        return httpd_resp_sendstr(req, e);
    }

    c.frm_enabled = en ? 1 : 0;
    c.frm_unit    = (uint8_t)unit;
    if (modbus_rtu_set_cfg(&c) != ESP_OK)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"Speichern fehlgeschlagen\"}");
    ESP_LOGI(TAG, "producer meter set via web: en=%d unit=%d serial %lu / %s",
             c.frm_enabled, unit, (unsigned long)c.frm_serial, c.frm_serial_txt);

    char buf[460];
    int o = snprintf(buf, sizeof(buf), "{\"ok\":true,\"st\":");
    o += frm_json(buf + o, sizeof(buf) - o - 2);
    snprintf(buf + o, sizeof(buf) - o, "}");
    return httpd_resp_sendstr(req, buf);
}

void deye_web_register(httpd_handle_t server)
{
    httpd_uri_t page  = { .uri = "/deye",       .method = HTTP_GET, .handler = page_handler };
    httpd_uri_t rd    = { .uri = "/deye/read",  .method = HTTP_GET, .handler = read_handler };
    httpd_uri_t wr    = { .uri = "/deye/write", .method = HTTP_GET, .handler = write_handler };
    httpd_uri_t live  = { .uri = "/api/deye/live", .method = HTTP_GET, .handler = live_handler };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &rd);
    httpd_register_uri_handler(server, &wr);
    httpd_register_uri_handler(server, &live);
    httpd_uri_t effr  = { .uri = "/api/deye/eff/reset", .method = HTTP_POST, .handler = eff_reset_handler };
    httpd_register_uri_handler(server, &effr);
    httpd_uri_t cg    = { .uri = "/api/deye/eff/curve", .method = HTTP_GET,  .handler = curve_get_handler };
    httpd_uri_t cp    = { .uri = "/api/deye/eff/curve", .method = HTTP_POST, .handler = curve_post_handler };
    httpd_register_uri_handler(server, &cg);
    httpd_register_uri_handler(server, &cp);
    httpd_uri_t og    = { .uri = "/api/ui/order", .method = HTTP_GET,  .handler = order_get_handler };
    httpd_uri_t op    = { .uri = "/api/ui/order", .method = HTTP_POST, .handler = order_post_handler };
    httpd_register_uri_handler(server, &og);
    httpd_register_uri_handler(server, &op);
    httpd_uri_t eg    = { .uri = "/api/em/*", .method = HTTP_GET,  .handler = em_get_handler };
    httpd_uri_t ep    = { .uri = "/api/em/*", .method = HTTP_POST, .handler = em_post_handler };
    httpd_register_uri_handler(server, &eg);
    httpd_register_uri_handler(server, &ep);
    httpd_uri_t frm   = { .uri = "/api/frm",     .method = HTTP_GET, .handler = frm_handler };
    httpd_uri_t frms  = { .uri = "/api/frm/set", .method = HTTP_GET, .handler = frm_set_handler };
    httpd_register_uri_handler(server, &frm);
    httpd_register_uri_handler(server, &frms);
    ESP_LOGI(TAG, "/deye routes registered");
}

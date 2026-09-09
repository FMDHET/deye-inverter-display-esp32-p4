#include "deye_web.h"
#include "modbus_rtu.h"
#include "deye_ctrl.h"
#include "webauth.h"

#include <stdarg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define LIVE_CAP 1600
static char s_live_json[LIVE_CAP];

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
    /* Battery mode: what is in force, how long it still has, and whether its
     * register writes actually verified -- a forced mode whose writes failed
     * used to look exactly like one that worked. */
    deye_ctrl_status_t cs;
    deye_ctrl_get_status(&cs);
    o = jcat(s_live_json, LIVE_CAP, o,
             "\"ctrl\":{\"mode\":%d,\"mode_name\":\"%s\",\"power\":%d,\"user_power\":%d,"
             "\"age\":%u,\"left\":%u,\"checked\":%u,\"failed\":%u}}",
             (int)cs.mode, deye_ctrl_mode_name(cs.mode), cs.power_w, cs.user_power_w,
             (unsigned)cs.age_s, (unsigned)cs.left_s,
             (unsigned)cs.checked, (unsigned)cs.failed);
    (void)o;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s_live_json);
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
    ESP_LOGI(TAG, "/deye routes registered");
}

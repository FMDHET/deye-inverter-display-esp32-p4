#include "meter_web.h"
#include "webauth.h"
#include "modbus_tcp.h"
#include "modbus_rtu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "meter_web";

/* esp_http_server dispatches requests from a single task, so handlers never run
 * concurrently -- a static response buffer avoids putting 3 kB on the (6 kB)
 * server stack. */
#define JSON_CAP  3072
/* Never start another device row without this much room left: the served +
 * manip tail that follows the loop is ~450 B and must always fit. */
#define JSON_TAIL 700
static char s_json[JSON_CAP];

/* Append to `buf` and return the new offset, saturating at cap-1 instead of
 * running past it. snprintf returns what it WOULD have written, so tracking the
 * offset with a bare `o += snprintf(...)` lets `o` overshoot `cap` and turns
 * the next `cap - o` into a huge size_t -- this keeps that impossible. */
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

/* Copy a user-configured label/IP into a JSON string body. Device names come
 * from the on-device settings UI, so a quote or backslash in one must not be
 * able to break the document. */
static void jesc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; src && *src && o + 2 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c >= 0x20)          { dst[o++] = (char)c; }
        /* control characters are simply dropped */
    }
    dst[o] = '\0';
}

/* The page itself lives in the /deye hub (deye.html, tab "meter"). Keep /meter
 * as a bookmarkable alias that redirects there rather than serving a second
 * copy of the same markup. */
static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/deye#meter");
    return httpd_resp_send(req, NULL, 0);
}

/* Parse a float query parameter, or `def` if absent/invalid. */
static float qparam_f(const char *q, const char *key, float def)
{
    char v[20];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return def;
    char *end = NULL;
    float f = strtof(v, &end);
    return (end && end != v) ? f : def;
}

static int qparam_i(const char *q, const char *key, int def)
{
    char v[20];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return def;
    return atoi(v);
}

/* GET /api/meter -> everything the page shows, in one poll. */
static esp_err_t values_handler(httpd_req_t *req)
{
    mb_dev_live_t dl[MB_MAX_DEVICES];
    int n = modbus_tcp_get_device_live(dl, MB_MAX_DEVICES);

    mb_served_t sv;
    modbus_rtu_get_served(&sv);
    mb_manip_cfg_t m;
    modbus_rtu_get_manip(&m);

    int o = 0;
    o = jcat(s_json, JSON_CAP, o, "{\"meters\":[");
    for (int i = 0; i < n; i++) {
        if (o > JSON_CAP - JSON_TAIL) break;
        mb_phases_t ph;
        bool has = modbus_tcp_get_phases(i, &ph);
        char name[2 * sizeof(dl[i].name)], ip[2 * sizeof(dl[i].ip)];
        jesc(name, sizeof(name), dl[i].name);
        jesc(ip,   sizeof(ip),   dl[i].ip);

        /* rc = numeric mb_role_t so the page can decide per role what "power"
         * means without matching German labels; pv/w/soc mirror /api/devices:
         * for a Fronius INVERTER the production is pv, w is the hybrid's
         * battery, soc its state of charge. ptot stays the meter total. */
        o = jcat(s_json, JSON_CAP, o,
                 "%s{\"idx\":%d,\"name\":\"%s\",\"ip\":\"%s\",\"slave\":%u,"
                 "\"mfr\":\"%s\",\"role\":\"%s\",\"rc\":%u,\"grid\":%d,\"conn\":%d,"
                 "\"phases\":%d,\"age\":%u,\"ptot\":%.0f,"
                 "\"pv\":%.0f,\"w\":%.0f,\"soc\":%.0f",
                 i ? "," : "", i, name, ip, dl[i].slave,
                 modbus_tcp_mfr_name(dl[i].mfr), modbus_tcp_role_name(dl[i].role),
                 dl[i].role, dl[i].role == MB_ROLE_GRID ? 1 : 0, dl[i].connected ? 1 : 0,
                 has ? 1 : 0, (unsigned)(has ? ph.age_ms : 0),
                 has ? ph.p_total : dl[i].w,
                 dl[i].pv_w, dl[i].w, dl[i].soc);
        if (has)
            o = jcat(s_json, JSON_CAP, o,
                     ",\"v\":[%.1f,%.1f,%.1f],\"i\":[%.2f,%.2f,%.2f],"
                     "\"p\":[%.0f,%.0f,%.0f]",
                     ph.v[0], ph.v[1], ph.v[2], ph.i[0], ph.i[1], ph.i[2],
                     ph.p[0], ph.p[1], ph.p[2]);
        o = jcat(s_json, JSON_CAP, o, "}");
    }
    o = jcat(s_json, JSON_CAP, o,
             "],\"served\":{\"fresh\":%d,\"per_phase\":%d,\"slave\":%d,"
             "\"quiet\":%d,\"hold\":%u,\"stale\":%u,"
             "\"sp\":%d,\"req\":%u,\"age\":%u,"
             "\"real\":[%.0f,%.0f,%.0f],\"real_total\":%.0f,"
             "\"p\":[%.0f,%.0f,%.0f],\"total\":%.0f,"
             "\"v\":[%.1f,%.1f,%.1f],\"i\":[%.2f,%.2f,%.2f]},",
             sv.fresh ? 1 : 0, sv.per_phase ? 1 : 0, sv.slave_running ? 1 : 0,
             sv.quiet ? 1 : 0, (unsigned)sv.hold_s, (unsigned)sv.stale_s,
             sv.setpoint, (unsigned)sv.requests, (unsigned)sv.age_ms,
             sv.real_p[0], sv.real_p[1], sv.real_p[2], sv.real_total,
             sv.served_p[0], sv.served_p[1], sv.served_p[2], sv.served_total,
             sv.served_v[0], sv.served_v[1], sv.served_v[2],
             sv.served_i[0], sv.served_i[1], sv.served_i[2]);
    o = jcat(s_json, JSON_CAP, o,
             "\"manip\":{\"en\":%d,\"ph\":[{\"mode\":%u,\"val\":%.1f},"
             "{\"mode\":%u,\"val\":%.1f},{\"mode\":%u,\"val\":%.1f}]}}",
             m.enabled ? 1 : 0,
             m.ph[0].mode, m.ph[0].value,
             m.ph[1].mode, m.ph[1].value,
             m.ph[2].mode, m.ph[2].value);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, s_json);
}

/* GET /api/meter/manip?en=&m1=&v1=&m2=&v2=&m3=&v3=[&sp=]
 *
 * Every parameter is optional and defaults to the value in force, so the page
 * can send a single knob. `sp` additionally moves the grid setpoint. */
static esp_err_t manip_handler(httpd_req_t *req)
{
    /* This is a WRITING endpoint: it changes what the inverter is told and
     * moves the grid setpoint. */
    if (!web_auth_ok(req)) return ESP_OK;

    char q[160];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"no query\"}");

    mb_manip_cfg_t m;
    modbus_rtu_get_manip(&m);

    m.enabled = qparam_i(q, "en", m.enabled) ? 1 : 0;
    for (int k = 0; k < 3; k++) {
        char km[4] = { 'm', (char)('1' + k), 0 };
        char kv[4] = { 'v', (char)('1' + k), 0 };
        int mode = qparam_i(q, km, m.ph[k].mode);
        if (mode < 0 || mode >= MB_PH_MODE_COUNT)
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"bad mode\"}");
        m.ph[k].mode  = (uint8_t)mode;
        m.ph[k].value = qparam_f(q, kv, m.ph[k].value);
    }

    /* Setpoint is a separate NVS value but belongs to the same control chain --
     * accept it here so the page has one place to steer from. */
    char spv[16];
    if (httpd_query_key_value(q, "sp", spv, sizeof(spv)) == ESP_OK) {
        int sp = atoi(spv);
        if (sp < -30000 || sp > 30000)
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"bad sp\"}");
        modbus_rtu_set_grid_setpoint(sp);
    }

    esp_err_t e = modbus_rtu_set_manip(&m);
    ESP_LOGW(TAG, "manip set via web: en=%u", m.enabled);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":%s}", e == ESP_OK ? "true" : "false");
    return httpd_resp_sendstr(req, buf);
}

void meter_web_register(httpd_handle_t server)
{
    httpd_uri_t page  = { .uri = "/meter",            .method = HTTP_GET, .handler = page_handler };
    httpd_uri_t vals  = { .uri = "/api/meter",        .method = HTTP_GET, .handler = values_handler };
    httpd_uri_t manip = { .uri = "/api/meter/manip",  .method = HTTP_GET, .handler = manip_handler };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &vals);
    httpd_register_uri_handler(server, &manip);
    ESP_LOGI(TAG, "/meter routes registered");
}

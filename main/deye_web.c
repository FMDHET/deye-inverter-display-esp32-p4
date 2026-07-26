#include "deye_web.h"
#include "modbus_rtu.h"

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

void deye_web_register(httpd_handle_t server)
{
    httpd_uri_t page  = { .uri = "/deye",       .method = HTTP_GET, .handler = page_handler };
    httpd_uri_t rd    = { .uri = "/deye/read",  .method = HTTP_GET, .handler = read_handler };
    httpd_uri_t wr    = { .uri = "/deye/write", .method = HTTP_GET, .handler = write_handler };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &rd);
    httpd_register_uri_handler(server, &wr);
    ESP_LOGI(TAG, "/deye routes registered");
}

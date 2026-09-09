#include "config_web.h"
#include "webauth.h"
#include "nvs_store.h"
#include "wifi_mgr.h"
#include "modbus_tcp.h"
#include "modbus_rtu.h"
#include "mqtt_fwd.h"
#include "ntp_client.h"
#include "wg_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "config_web";

/* The whole document, both directions. 8 kB is roughly twice what a full setup
 * produces (10 WiFi networks and 8 Modbus devices are the caps). */
#define DOC_CAP  8192

/* Every settings blob NVS holds, with the size the owning module expects. The
 * blobs go out as base64 of the RAW bytes instead of field-by-field JSON: it
 * round-trips exactly, cannot half-apply, and needs no second schema that would
 * then have to be kept in step with the structs. This is a backup file, not a
 * config format meant for hand-editing -- the scalars below are readable, the
 * blobs are not, and that is the trade. */
typedef struct {
    const char *key;
    size_t      size;      /* 0 = variable length (list blobs) */
    size_t      cap;       /* buffer to read into               */
    esp_err_t (*get_fixed)(void *buf, size_t len);
    esp_err_t (*set_fixed)(const void *buf, size_t len);
    esp_err_t (*get_var)(void *buf, size_t *len);
    esp_err_t (*set_var)(const void *buf, size_t len);
} blob_t;

static const blob_t BLOBS[] = {
    { "mb_devices", 0, MB_MAX_DEVICES * sizeof(mb_dev_cfg_t), NULL, NULL,
      nvs_store_get_mb_devices, nvs_store_set_mb_devices },
    { "wifi_list",  0, WIFI_MGR_MAX_CREDS * sizeof(wifi_cred_t), NULL, NULL,
      nvs_store_get_wifi_list, nvs_store_set_wifi_list },
    { "mb_rtu",   sizeof(mb_rtu_cfg_t),   sizeof(mb_rtu_cfg_t),
      nvs_store_get_mb_rtu,   nvs_store_set_mb_rtu,   NULL, NULL },
    { "mb_manip", sizeof(mb_manip_cfg_t), sizeof(mb_manip_cfg_t),
      nvs_store_get_mb_manip, nvs_store_set_mb_manip, NULL, NULL },
    { "mqtt",     sizeof(mqtt_cfg_t),     sizeof(mqtt_cfg_t),
      nvs_store_get_mqtt,     nvs_store_set_mqtt,     NULL, NULL },
    { "ntp",      sizeof(ntp_cfg_t),      sizeof(ntp_cfg_t),
      nvs_store_get_ntp,      nvs_store_set_ntp,      NULL, NULL },
    { "wg",       sizeof(wg_cfg_t),       sizeof(wg_cfg_t),
      nvs_store_get_wg,       nvs_store_set_wg,       NULL, NULL },
};
#define N_BLOBS (sizeof(BLOBS) / sizeof(BLOBS[0]))

/* Saturating append, same as the jcat() helpers elsewhere: a bare
 * `o += snprintf()` lets o run past the buffer, and `doc + o` is then a pointer
 * past the end even if nothing is written. */
static int dcat(char *buf, size_t cap, int o, const char *fmt, ...)
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

/* --------------------------------- GET --------------------------------- */

static esp_err_t export_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;    /* contains every secret -- gated */

    char *doc = heap_caps_malloc(DOC_CAP, MALLOC_CAP_SPIRAM);
    uint8_t *raw = heap_caps_malloc(MB_MAX_DEVICES * sizeof(mb_dev_cfg_t) + 64,
                                    MALLOC_CAP_SPIRAM);
    char *b64 = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!doc || !raw || !b64) {
        free(doc); free(raw); free(b64);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    char ap[32] = "", pw[WEB_AUTH_PW_MAX] = "";
    nvs_store_get_ap_psk(ap, sizeof(ap));
    web_auth_get(pw, sizeof(pw));

    int o = dcat(doc, DOC_CAP, 0,
        "{\n  \"format\": 1,\n  \"device\": \"deye-display\",\n"
        "  \"note\": \"Sicherung der Einstellungen. Enthaelt Passwoerter und den "
        "privaten WireGuard-Schluessel -- entsprechend aufbewahren. "
        "Zurueckspielen: POST /config mit dieser Datei.\",\n"
        "  \"scalars\": {\n"
        "    \"brightness\": %u, \"contrast\": %u, \"sleep_secs\": %u, \"orientation\": %u,\n"
        "    \"grid_sp\": %d, \"sls_a\": %u,\n"
        "    \"ap_psk\": \"%s\", \"web_pw\": \"%s\"\n  },\n  \"blobs\": {\n",
        nvs_store_get_brightness(), nvs_store_get_contrast(),
        nvs_store_get_sleep_secs(), nvs_store_get_orientation(),
        nvs_store_get_grid_sp(), nvs_store_get_sls_a(), ap, pw);

    for (size_t i = 0; i < N_BLOBS; i++) {
        const blob_t *b = &BLOBS[i];
        size_t len = b->cap;
        memset(raw, 0, b->cap);
        bool have;
        if (b->get_var) have = (b->get_var(raw, &len) == ESP_OK);
        else            { len = b->size; have = (b->get_fixed(raw, len) == ESP_OK); }

        size_t olen = 0;
        if (have && len > 0 &&
            mbedtls_base64_encode((unsigned char *)b64, 2048, &olen, raw, len) == 0) {
            o = dcat(doc, DOC_CAP, o, "    \"%s\": \"%.*s\"%s\n",
                     b->key, (int)olen, b64, i + 1 < N_BLOBS ? "," : "");
        } else {
            o = dcat(doc, DOC_CAP, o, "    \"%s\": null%s\n",
                     b->key, i + 1 < N_BLOBS ? "," : "");
        }
        if ((size_t)o >= DOC_CAP - 8) {   /* would be truncated JSON */
            free(doc); free(raw); free(b64);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config too large");
            return ESP_FAIL;
        }
    }
    o = dcat(doc, DOC_CAP, o, "  }\n}\n");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"deye-display-config.json\"");
    esp_err_t e = httpd_resp_send(req, doc, o);
    ESP_LOGW(TAG, "settings exported (%d bytes)", o);
    free(doc); free(raw); free(b64);
    return e;
}

/* --------------------------------- POST -------------------------------- */

static void apply_scalars(const cJSON *sc, int *applied)
{
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(sc, "brightness")) && cJSON_IsNumber(v))
        { nvs_store_set_brightness((uint8_t)v->valueint); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "contrast")) && cJSON_IsNumber(v))
        { nvs_store_set_contrast((uint8_t)v->valueint); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "sleep_secs")) && cJSON_IsNumber(v))
        { nvs_store_set_sleep_secs((uint16_t)v->valueint); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "orientation")) && cJSON_IsNumber(v))
        { nvs_store_set_orientation((uint8_t)v->valueint); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "grid_sp")) && cJSON_IsNumber(v))
        { nvs_store_set_grid_sp(v->valueint); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "sls_a")) && cJSON_IsNumber(v))
        { nvs_store_set_sls_a((uint8_t)v->valueint); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "ap_psk")) && cJSON_IsString(v) && v->valuestring[0])
        { nvs_store_set_ap_psk(v->valuestring); (*applied)++; }
    if ((v = cJSON_GetObjectItem(sc, "web_pw")) && cJSON_IsString(v))
        { web_auth_set(v->valuestring); (*applied)++; }
}

static esp_err_t import_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;

    if (req->content_len <= 0 || req->content_len >= DOC_CAP) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_FAIL;
    }
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    uint8_t *raw = heap_caps_malloc(MB_MAX_DEVICES * sizeof(mb_dev_cfg_t) + 64,
                                    MALLOC_CAP_SPIRAM);
    if (!body || !raw) {
        free(body); free(raw);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); free(raw);
                      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
                      return ESP_FAIL; }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body); free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not JSON");
        return ESP_FAIL;
    }

    /* Refuse a document that is not ours: restoring a stranger's file would
     * overwrite the WiFi credentials of the device you are talking to. */
    const cJSON *dev = cJSON_GetObjectItem(root, "device");
    if (!cJSON_IsString(dev) || strcmp(dev->valuestring, "deye-display") != 0) {
        cJSON_Delete(root); free(body); free(raw);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "not a deye-display config (field \"device\")");
        return ESP_FAIL;
    }

    int applied = 0, skipped = 0;
    const cJSON *sc = cJSON_GetObjectItem(root, "scalars");
    if (cJSON_IsObject(sc)) apply_scalars(sc, &applied);

    const cJSON *bl = cJSON_GetObjectItem(root, "blobs");
    for (size_t i = 0; i < N_BLOBS && cJSON_IsObject(bl); i++) {
        const blob_t *b = &BLOBS[i];
        const cJSON *v = cJSON_GetObjectItem(bl, b->key);
        if (!cJSON_IsString(v)) { skipped++; continue; }
        size_t len = 0;
        if (mbedtls_base64_decode(raw, b->cap, &len,
                                  (const unsigned char *)v->valuestring,
                                  strlen(v->valuestring)) != 0 || len == 0) {
            ESP_LOGW(TAG, "%s: base64 rejected -- skipped", b->key);
            skipped++;
            continue;
        }
        /* A fixed-size blob must match exactly. A shorter one would be a
         * config from a firmware with fewer fields; the owning module handles
         * that on load, but writing it straight through would strip the newer
         * fields silently -- so refuse instead of half-restoring. */
        if (b->size && len != b->size) {
            ESP_LOGW(TAG, "%s: %u bytes, expected %u -- skipped",
                     b->key, (unsigned)len, (unsigned)b->size);
            skipped++;
            continue;
        }
        esp_err_t e = b->set_var ? b->set_var(raw, len) : b->set_fixed(raw, len);
        if (e == ESP_OK) applied++;
        else { ESP_LOGE(TAG, "%s: write failed (%s)", b->key, esp_err_to_name(e)); skipped++; }
    }

    cJSON_Delete(root);
    free(body); free(raw);

    ESP_LOGW(TAG, "settings imported: %d applied, %d skipped -- reboot to take effect",
             applied, skipped);

    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"ok\":true,\"applied\":%d,\"skipped\":%d,"
             "\"note\":\"Neustart noetig (POST /ota/reboot)\"}", applied, skipped);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, msg);
}

void config_web_register(httpd_handle_t server)
{
    httpd_uri_t get  = { .uri = "/config", .method = HTTP_GET,  .handler = export_handler };
    httpd_uri_t post = { .uri = "/config", .method = HTTP_POST, .handler = import_handler };
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
}

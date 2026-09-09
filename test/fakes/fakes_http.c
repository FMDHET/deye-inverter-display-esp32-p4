/* Fakes for esp_http_server and base64, kept apart from fakes.c so a test that
 * only needs the Modbus side does not drag the web stack in. */

#include <stdio.h>
#include <string.h>
#include <strings.h>     /* strcasecmp on glibc */
#include <sys/types.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "mbedtls/base64.h"

char fake_req_auth_hdr[256];
char fake_resp_status[64];
char fake_resp_www_auth[128];
char fake_resp_body[512];
int  fake_resp_sends;

void fake_http_reset(void)
{
    fake_req_auth_hdr[0] = '\0';
    fake_resp_status[0]  = '\0';
    fake_resp_www_auth[0] = '\0';
    fake_resp_body[0]    = '\0';
    fake_resp_sends      = 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field,
                                      char *val, size_t val_size)
{
    (void)r;
    if (strcasecmp(field, "Authorization") != 0) return ESP_ERR_NOT_FOUND;
    if (fake_req_auth_hdr[0] == '\0')            return ESP_ERR_NOT_FOUND;
    if (strlen(fake_req_auth_hdr) >= val_size)   return ESP_ERR_INVALID_SIZE;
    strcpy(val, fake_req_auth_hdr);
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    snprintf(fake_resp_status, sizeof(fake_resp_status), "%s", status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    if (strcasecmp(field, "WWW-Authenticate") == 0)
        snprintf(fake_resp_www_auth, sizeof(fake_resp_www_auth), "%s", value);
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    (void)r;
    fake_resp_sends++;
    snprintf(fake_resp_body, sizeof(fake_resp_body), "%s", str ? str : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len)
{
    (void)r; (void)len;
    fake_resp_sends++;
    if (buf) snprintf(fake_resp_body, sizeof(fake_resp_body), "%s", buf);
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    (void)r; (void)buf; (void)len;
    fake_resp_sends++;
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg)
{
    (void)r;
    snprintf(fake_resp_status, sizeof(fake_resp_status), "%d", code);
    snprintf(fake_resp_body, sizeof(fake_resp_body), "%s", msg ? msg : "");
    fake_resp_sends++;
    return ESP_OK;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    (void)r; (void)buf; (void)len;
    return ESP_ERR_NOT_FOUND;               /* no query unless a test needs one */
}
esp_err_t httpd_query_key_value(const char *qry, const char *key,
                                char *val, size_t val_size)
{
    (void)qry; (void)key; (void)val; (void)val_size;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u)
{
    (void)h; (void)u; return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    (void)r; (void)buf; (void)len; return 0;
}

/* ------------------------------- base64 -------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    size_t need = ((slen + 2) / 3) * 4;
    if (olen) *olen = need;
    if (dlen < need + 1) return -0x2A;               /* BUFFER_TOO_SMALL */
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        unsigned v = src[i] << 16;
        if (i + 1 < slen) v |= src[i + 1] << 8;
        if (i + 2 < slen) v |= src[i + 2];
        dst[o++] = B64[(v >> 18) & 63];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = (i + 1 < slen) ? B64[(v >> 6) & 63] : '=';
        dst[o++] = (i + 2 < slen) ? B64[v & 63]        : '=';
    }
    dst[o] = '\0';
    return 0;
}

static int b64_val(unsigned char c)
{
    const char *p = strchr(B64, c);
    return (c && p) ? (int)(p - B64) : -1;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = src[i];
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = b64_val(c);
        if (v < 0) return -0x2C;                     /* INVALID_CHARACTER */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= dlen) return -0x2A;             /* BUFFER_TOO_SMALL */
            dst[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    if (olen) *olen = o;
    return 0;
}

#include "applog.h"
#include "webauth.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "applog";

/* 48 kB of PSRAM ~= 500 log lines, enough to cover a boot plus a few minutes of
 * operation. It lives in PSRAM on purpose: internal RAM is the scarce, DMA-able
 * kind (see the OTA story in ota.c), and 48 kB of it would come straight out of
 * the transport buffers. PSRAM is safe here because this hook only ever runs in
 * task context -- ESP_EARLY_LOG and anything logging from an ISR go through
 * esp_rom_printf and never reach us -- so it is never called with the flash
 * cache disabled. */
#define LOG_CAP  (48 * 1024)
#define LOG_LINE_MAX 256

static char           *s_buf;
static size_t          s_len;     /* bytes in use (grows to LOG_CAP, then wraps) */
static size_t          s_head;    /* write position                              */
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t  s_next;    /* the UART writer we replaced                 */

static void ring_push(const char *p, size_t n)
{
    if (n == 0 || !s_buf) return;
    if (n > LOG_CAP) { p += n - LOG_CAP; n = LOG_CAP; }   /* keep the tail */

    portENTER_CRITICAL(&s_mux);
    size_t first = LOG_CAP - s_head;
    if (first > n) first = n;
    memcpy(s_buf + s_head, p, first);
    if (n > first) memcpy(s_buf, p + first, n - first);   /* wrapped */
    s_head = (s_head + n) % LOG_CAP;
    s_len += n;
    if (s_len > LOG_CAP) s_len = LOG_CAP;
    portEXIT_CRITICAL(&s_mux);
}

/* Formats the line twice: once for the UART (unchanged, by the writer we
 * replaced) and once into our own buffer. va_list is single-use, hence the
 * va_copy -- reusing it after the first call is undefined behaviour and does
 * bite on RISC-V. */
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = s_next ? s_next(fmt, ap) : 0;

    char line[LOG_LINE_MAX];
    int m = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (m > 0) ring_push(line, (size_t)m < sizeof(line) - 1 ? (size_t)m : sizeof(line) - 1);
    return n;
}

esp_err_t applog_init(void)
{
    if (s_buf) return ESP_OK;
    s_buf = heap_caps_malloc(LOG_CAP, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the log ring -- /log stays empty");
        return ESP_ERR_NO_MEM;
    }
    s_next = esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "log ring up (%d kB in PSRAM), GET /log", LOG_CAP / 1024);
    return ESP_OK;
}

/* ------------------------------- GET /log ------------------------------ */

static esp_err_t log_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;     /* 401 already sent */

    size_t want = LOG_CAP, clear = 0;
    char q[48], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "tail", v, sizeof(v)) == ESP_OK) {
            long t = strtol(v, NULL, 10);
            if (t > 0) want = (size_t)t;
        }
        if (httpd_query_key_value(q, "clear", v, sizeof(v)) == ESP_OK) clear = (v[0] == '1');
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!s_buf) return httpd_resp_sendstr(req, "log ring not allocated\n");

    /* Snapshot the geometry, then send in chunks WITHOUT holding the lock: a
     * send can block on the socket for seconds, and the whole device logs into
     * this buffer. A line that arrives mid-send is simply not in this answer. */
    portENTER_CRITICAL(&s_mux);
    size_t len = s_len, head = s_head;
    portEXIT_CRITICAL(&s_mux);

    if (want < len) len = want;
    size_t start = (head + LOG_CAP - len) % LOG_CAP;   /* oldest kept byte */

    char chunk[512];
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        size_t at = (start + sent) % LOG_CAP;
        size_t first = LOG_CAP - at;
        if (first > n) first = n;
        portENTER_CRITICAL(&s_mux);
        memcpy(chunk, s_buf + at, first);
        if (n > first) memcpy(chunk + first, s_buf, n - first);
        portEXIT_CRITICAL(&s_mux);
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) return ESP_FAIL;
        sent += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);

    if (clear) {
        portENTER_CRITICAL(&s_mux);
        s_len = 0; s_head = 0;
        portEXIT_CRITICAL(&s_mux);
    }
    return ESP_OK;
}

void applog_register(httpd_handle_t server)
{
    httpd_uri_t log = { .uri = "/log", .method = HTTP_GET, .handler = log_handler };
    httpd_register_uri_handler(server, &log);
}

#include "https_get.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "https_get";

/* A handshake costs internal RAM (measured 2026-10-21: lwIP buffers and many
 * small blocks while the certificate chain arrives, tens of kB at the peak).
 * Starting one when internal DMA memory is already low -- an OTA upload, the
 * web mirror streaming -- is how the esp_hosted driver once starved. So a
 * request only starts with this much headroom; otherwise the feed backs off
 * and tries again later. */
#define HTTPS_MIN_INTERNAL_FREE (45 * 1024)
#define HTTPS_WAIT_POLL_MS      500
#define HTTPS_WAIT_POLLS        60      /* up to 30 s */

static SemaphoreHandle_t s_lock;
static portMUX_TYPE      s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static bool lock_ready(void)
{
    portENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&s_init_mux);
    return s_lock != NULL;
}

bool https_pause(uint32_t wait_ms)
{
    return lock_ready() && xSemaphoreTake(s_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void https_resume(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

esp_err_t https_get(const https_req_t *rq, char *buf, size_t cap, size_t *len, int *status)
{
    if (len) *len = 0;
    if (status) *status = 0;
    if (!rq || !rq->url || !buf || cap < 2) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    if (!lock_ready() || xSemaphoreTake(s_lock, pdMS_TO_TICKS(60000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    /* Internal free swings by 10-15 kB within seconds (measured 38-52 kB in
     * normal operation), so a single check right at the threshold fails about
     * every other time. Wait for a good moment instead of giving up at once. */
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    for (int i = 0; i < HTTPS_WAIT_POLLS && free_int < HTTPS_MIN_INTERNAL_FREE; i++) {
        vTaskDelay(pdMS_TO_TICKS(HTTPS_WAIT_POLL_MS));
        free_int = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (free_int < HTTPS_MIN_INTERNAL_FREE) {
        ESP_LOGW(TAG, "%.48s... postponed: %u B internal free (< %u)", rq->url,
                 (unsigned)free_int, (unsigned)HTTPS_MIN_INTERNAL_FREE);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url               = rq->url,
        .method            = rq->post_json ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms        = rq->timeout_ms > 0 ? rq->timeout_ms : 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
        .user_agent        = "deye-display",
    };
    esp_err_t e = ESP_FAIL;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }

    char auth[160];
    if (rq->auth_bearer) {
        snprintf(auth, sizeof(auth), "Bearer %s", rq->auth_bearer);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    int body_len = 0;
    if (rq->post_json) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        body_len = (int)strlen(rq->post_json);
    }
    esp_http_client_set_header(c, "Accept", "application/json");

    e = esp_http_client_open(c, body_len);
    if (e == ESP_OK && body_len) {
        if (esp_http_client_write(c, rq->post_json, body_len) != body_len) e = ESP_FAIL;
    }
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (status) *status = st;
        size_t got = 0;
        for (;;) {
            if (got >= cap - 1) {               /* still more? then it did not fit */
                char probe;
                if (esp_http_client_read(c, &probe, 1) > 0) e = ESP_ERR_INVALID_SIZE;
                break;
            }
            int r = esp_http_client_read(c, buf + got, (int)(cap - 1 - got));
            if (r < 0) { e = ESP_FAIL; break; }
            if (r == 0) break;
            got += (size_t)r;
        }
        buf[got] = '\0';
        if (len) *len = got;
        if (e == ESP_OK && st != 200) e = ESP_FAIL;
        if (e != ESP_OK)
            ESP_LOGW(TAG, "%.48s... -> HTTP %d, %u bytes (%s)", rq->url, st, (unsigned)got, esp_err_to_name(e));
    } else {
        ESP_LOGW(TAG, "%.48s... -> no connection (%s)", rq->url, esp_err_to_name(e));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    xSemaphoreGive(s_lock);
    return e;
}

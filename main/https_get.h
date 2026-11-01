#pragma once

/* One HTTPS request, the way the energy manager's feeds need it: small JSON
 * answers (a few to ~30 kB) from public APIs, verified against the IDF
 * certificate bundle. Blocking -- call it from a task of your own, never from
 * the HTTP server or the LVGL task.
 *
 * Requests are serialised by a mutex: one TLS session at a time keeps the
 * memory peak of the handshake bounded (this board's WiFi runs over esp_hosted
 * SDIO, whose buffers once starved when internal RAM ran short). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *url;
    const char *auth_bearer;   /* NULL = no Authorization header           */
    const char *post_json;     /* NULL = GET, else POST with this JSON body */
    int         timeout_ms;    /* 0 = 15000                                 */
} https_req_t;

/* Fetch into buf (NUL-terminated). Returns ESP_OK only for HTTP 200 with a
 * body that fit; *status gets the HTTP status (0 when no answer), *len the
 * body length. ESP_ERR_INVALID_SIZE when the body did not fit into cap-1;
 * ESP_ERR_NO_MEM when internal RAM stayed too low for a handshake for 30 s
 * (nothing was sent -- try again later). */
esp_err_t https_get(const https_req_t *rq, char *buf, size_t cap, size_t *len, int *status);

/* Hold off all HTTPS requests -- for a flash upload. A TLS handshake is
 * seconds of ECC arithmetic on CPU 0; while an upload is written to flash,
 * CPU 0 is parked for every flash operation, the handshake stretches, and the
 * idle task on CPU 0 never got to run: two task-watchdog resets mid-upload on
 * 2026-10-21 (core dumps of builds 341 and 345). https_pause() waits up to
 * wait_ms for a running request to end and keeps new ones waiting until
 * https_resume(). Returns false if it could not get hold of the lock. */
bool https_pause(uint32_t wait_ms);
void https_resume(void);

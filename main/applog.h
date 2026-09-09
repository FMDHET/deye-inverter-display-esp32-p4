#pragma once

/* Live log ring buffer + GET /log.
 *
 * The device hangs on a wall: there is no serial console when something goes
 * odd, and until now `reset` in /ota was the only thing a crash left behind.
 * This keeps the last few dozen kilobytes of log lines in RAM and hands them
 * out over HTTP, so "what was it doing just before?" is answerable remotely.
 *
 * It does NOT survive a reboot -- that is what the coredump partition is for
 * (see ota.c: /ota reports task, PC and reason, GET /coredump hands out the
 * image for espcoredump.py). The two together cover "what happened".
 */

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the ring and hook esp_log. Call as early as possible in app_main --
 * everything logged before this is only on the UART. */
esp_err_t applog_init(void);

/* GET /log -> the buffer as text/plain, oldest line first.
 *   ?tail=<n>   only the last n bytes
 *   ?clear=1    empty the buffer after sending
 * Registered by captive.c together with the other routes. */
void      applog_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#pragma once

/* /deye web page: read & write the Deye inverter's Modbus holding registers
 * from a browser. Reads/writes are served by the Deye-master RTU bus task (see
 * modbus_rtu_deye_read/write) so they never collide with the regular poll.
 *
 * /api/deye/live serves the cached live measurements the same bus task polls
 * anyway (mb_deye_live_t) -- no RTU traffic per request, so any number of open
 * browsers costs the RS485 bus nothing. */

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register /deye (page), /deye/read, /deye/write and /api/deye/live on an
 * existing HTTP server (the captive :80 server). */
void deye_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

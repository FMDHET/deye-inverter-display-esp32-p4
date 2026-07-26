#pragma once

/* /deye web page: read & write the Deye inverter's Modbus holding registers
 * from a browser. Reads/writes are served by the Deye-master RTU bus task (see
 * modbus_rtu_deye_read/write) so they never collide with the regular poll. */

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register /deye (page), /deye/read and /deye/write on an existing HTTP server
 * (the captive :80 server). */
void deye_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

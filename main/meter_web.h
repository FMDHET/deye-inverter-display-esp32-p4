#pragma once

/* Meter API: live values of the Eltako meter(s) next to the values the SDM630
 * emulation actually serves to the Deye -- and per-phase overrides.
 *
 * The UI is the "Zaehler & Deye" tab of the /deye hub page (deye.html); /meter
 * only redirects there. The tab is a table with one row per phase: what the
 * meter measured, what the setpoint and the manipulation made of it, and what
 * the inverter is told. The overrides are the same knobs modbus_rtu_set_manip()
 * takes; they are persisted and steer the inverter, so the page carries the
 * warning that goes with that.
 */

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register /meter (redirect to the hub), /api/meter (values) and
 * /api/meter/manip (overrides) on an existing HTTP server (the captive :80
 * server). */
void meter_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

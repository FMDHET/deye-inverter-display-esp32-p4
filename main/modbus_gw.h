#pragma once

/* Modbus-TCP <-> RTU bridge (the ESP32 as a Modbus gateway).
 *
 * Counterpart to modbus_tcp.c: that one is a CLIENT polling foreign devices,
 * this one is a SERVER. It listens on TCP (default port 502), unwraps the MBAP
 * header of each request and forwards the PDU onto an RS485 bus via
 * modbus_rtu_txn(), then wraps the RTU answer back into MBAP. So a PC tool,
 * Home Assistant or any Modbus master on the LAN can read and write the Deye's
 * registers through the display, without a second RS485 adapter.
 *
 * - MULTI-RTU: both RS485 buses can be bridged at once; modbus_rtu_gw_route()
 *   maps the TCP unit id onto a bus (see modbus_rtu.h for the rule).
 * - MULTI-CLIENT: several TCP connections are served concurrently from one
 *   task via select(). The RS485 exchanges themselves are serialised per bus,
 *   which is what the wire requires anyway.
 *
 * Configured in the "Mod RTU" settings tab, persisted in mb_rtu_cfg_t.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime facts only -- the configured port and client limit stay in
 * mb_rtu_cfg_t (modbus_rtu_get_cfg()) rather than being mirrored here, so there
 * is nothing to keep in sync. */
typedef struct {
    bool     running;      /* socket bound and listening         */
    uint8_t  clients;      /* currently connected                */
    uint32_t req_ok;       /* requests answered from the RTU bus */
    uint32_t req_err;      /* answered with a gateway exception  */
    /* Connections that never became a client: accept() failed (the shared lwIP
     * socket pool was empty) or the slot limit was hit. Counted separately from
     * req_err because a client that cannot even connect never sends a request --
     * without this the bridge looks perfectly healthy while the LAN sees a
     * refused port. */
    uint32_t conn_rej;
    int32_t  last_errno;   /* errno of the last failed accept(); 0 = none */
} modbus_gw_status_t;

/* Starts the server task. Call AFTER modbus_rtu_start() -- it uses that
 * module's per-bus transaction slots. The task idles cheaply while the bridge
 * is switched off and picks up config changes within ~0.5 s. */
esp_err_t modbus_gw_start(void);

void      modbus_gw_get_status(modbus_gw_status_t *out);

#ifdef __cplusplus
}
#endif

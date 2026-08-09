#pragma once

/* Modbus-RTU over two RS485 buses (auto-direction transceivers).
 *
 * Each bus is independently configurable as MASTER or SLAVE:
 *   - SLAVE  -> emulate an Eastron SDM630. The Deye polls this bus and the ESP
 *              answers with the real grid power obtained over Modbus-TCP from
 *              the Fronius smart meter (modbus_tcp_get_status().grid_w).
 *   - MASTER -> read the Deye inverter (battery power + SoC) and feed it into
 *              the energy-flow model via modbus_tcp_set_rtu_deye().
 *
 * Bus 0 = UART1 (board pins A), bus 1 = UART2 (board pins B). Config is stored
 * in NVS and edited in the "Mod RTU" settings tab. Pins are fixed in the board
 * header.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_RTU_BUSES 2

typedef enum {
    MB_RTU_MASTER = 0,   /* read the Deye               */
    MB_RTU_SLAVE  = 1,   /* emulate an Eastron SDM630   */
    MB_RTU_ROLE_COUNT
} mb_rtu_role_t;

typedef struct {
    uint8_t  enabled;
    uint8_t  role;       /* mb_rtu_role_t                                  */
    uint8_t  slave_id;   /* master: Deye id to poll; slave: id we answer to */
    uint32_t baud;       /* default 9600                                   */
} mb_rtu_bus_cfg_t;

/* NOTE: fields are APPENDED AT TOP LEVEL (never inside mb_rtu_bus_cfg_t, whose
 * size is the stride of bus[] -- growing it would shift bus[1] and silently
 * corrupt every stored config). nvs_store_get_mb_rtu() accepts a shorter blob
 * and the caller pre-zeroes, so an old 16-byte record migrates in place with
 * the new fields reading 0 -> defaults below. */
typedef struct {
    mb_rtu_bus_cfg_t bus[MB_RTU_BUSES];      /* 2 x 8 = 16 B (frozen layout) */

    /* ---- Modbus-TCP <-> RTU bridge (appended) ---- */
    uint8_t  gw_enabled;      /* 0/1 master switch for the TCP server        */
    uint8_t  gw_bus_mask;     /* bit i set -> bus i is reachable via TCP     */
    uint16_t gw_port;         /* listen port (0 -> MB_GW_DEFAULT_PORT)       */
    uint16_t gw_timeout_ms;   /* RTU response timeout (0 -> default 800)     */
    uint8_t  gw_max_clients;  /* concurrent TCP clients (0 -> default 4)     */
    uint8_t  _rsv;
} mb_rtu_cfg_t;

#define MB_GW_DEFAULT_PORT       502
#define MB_GW_DEFAULT_TIMEOUT_MS 800
/* Client slots are LWIP sockets out of a pool (CONFIG_LWIP_MAX_SOCKETS = 16)
 * shared with two HTTP servers, DNS, MQTT, WireGuard and the Modbus-TCP poll
 * workers. That pool CANNOT be grown -- raising it starves the DMA-capable
 * internal RAM that esp_hosted's SDIO transport needs and the device no longer
 * boots (see the note in sdkconfig.defaults). So keep the bridge frugal: it is
 * off by default, and even fully loaded it takes at most 1 listener + 4
 * clients. RS485 is serial anyway, so more clients would only queue. */
#define MB_GW_DEFAULT_CLIENTS    2
#define MB_GW_MAX_CLIENTS        4      /* hard ceiling for gw_max_clients   */

typedef struct {
    bool     running;
    uint8_t  role;
    bool     online;     /* master: Deye responded                         */
    uint32_t polls;      /* master: ok reads; slave: requests served       */
    uint32_t errs;
    float    a, b;       /* master: deye_w, deye_soc; slave: grid_w served  */
} mb_rtu_bus_status_t;

typedef struct {
    mb_rtu_bus_status_t bus[MB_RTU_BUSES];
} mb_rtu_status_t;

esp_err_t   modbus_rtu_start(void);
void        modbus_rtu_get_status(mb_rtu_status_t *out);

/* Grid setpoint (W) for the zero-export trick: the SLAVE emulation reports
 * (real grid - setpoint) to the Deye, so the Deye drives the real grid point to
 * the setpoint. Set from the main-screen slider; persisted in NVS. */
int         modbus_rtu_get_grid_setpoint(void);
void        modbus_rtu_set_grid_setpoint(int w);

/* On-demand Deye holding-register access for the /deye web page, served by the
 * Deye-master bus task between polls (no UART contention). Read up to 64 regs
 * (FC03) or write one (FC06). Return 0 on success, negative on error/timeout. */
int         modbus_rtu_deye_read(uint16_t addr, uint16_t count, uint16_t *out);
int         modbus_rtu_deye_write(uint16_t addr, uint16_t val);
void        modbus_rtu_get_cfg(mb_rtu_cfg_t *out);
esp_err_t   modbus_rtu_set_cfg(const mb_rtu_cfg_t *cfg);
const char *modbus_rtu_role_name(uint8_t role);

/* ---------------- TCP <-> RTU bridge back-end -------------------------
 * modbus_gw.c accepts Modbus-TCP clients and hands each request here. The
 * RS485 UART stays owned by its bus task: a request is parked in a per-bus
 * slot, the bus task runs it between its own polls, and the caller blocks on
 * a semaphore. That keeps the timing-critical Eastron emulation and the Deye
 * control writes on their existing path -- the bridge only borrows the bus. */

#define MB_RTU_ADU_MAX 256   /* RTU frame incl. CRC */

/* Run one raw RTU transaction on `bus`.
 *   req  = [slave id][PDU...]   WITHOUT CRC (it is appended here)
 *   resp = [slave id][PDU...]   WITHOUT CRC (it is verified and stripped)
 * Returns the response length (>0) or a negative error. Only buses that
 * modbus_rtu_bus_can_gateway() accepts are served. */
int  modbus_rtu_txn(int bus, const uint8_t *req, int req_len,
                    uint8_t *resp, int resp_max, uint32_t timeout_ms);

/* True when `bus` may carry bridge traffic: the bridge is on, the bus is
 * flagged in gw_bus_mask, and the bus is an ENABLED MASTER. A SLAVE bus is
 * refused on purpose -- there the Deye is the master and our injected request
 * would collide with its polling. */
bool modbus_rtu_bus_can_gateway(int bus);

/* Map a Modbus-TCP unit id onto a bus, or -1 if nothing serves it:
 *   1. a gateway bus whose configured slave_id equals `unit` wins, so two
 *      buses can be addressed side by side (multi-RTU);
 *   2. otherwise, if exactly ONE bus is gateway-enabled, it takes every unit
 *      id, which lets a multi-drop RS485 segment be reached transparently. */
int  modbus_rtu_gw_route(uint8_t unit);

/* Self-test: physically connect bus A and bus B RS485 lines together
 * (A-TX→B-RX, A-RX→B-TX), then call modbus_rtu_selftest_start().
 * Bus B (master UART) sends an FC03 request addressed to bus A's slave_id;
 * bus A's slave task responds; bus B verifies the response.
 * Poll modbus_rtu_selftest_result() for the outcome. */
typedef enum {
    MB_RTU_SELFTEST_IDLE    = 0,
    MB_RTU_SELFTEST_PENDING,    /* request queued, not yet running  */
    MB_RTU_SELFTEST_PASS,
    MB_RTU_SELFTEST_FAIL,
} mb_rtu_selftest_state_t;

typedef struct {
    mb_rtu_selftest_state_t state;
    int32_t latency_ms;         /* round-trip time; -1 if not measured */
    char    error[80];
} mb_rtu_selftest_result_t;

void                     modbus_rtu_selftest_start(void);
mb_rtu_selftest_result_t modbus_rtu_selftest_result(void);

#ifdef __cplusplus
}
#endif

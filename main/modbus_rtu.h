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

typedef struct {
    mb_rtu_bus_cfg_t bus[MB_RTU_BUSES];
} mb_rtu_cfg_t;

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

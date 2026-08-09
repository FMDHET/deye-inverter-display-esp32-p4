#include "modbus_rtu.h"
#include "modbus_tcp.h"      /* grid_w source + RTU-Deye sink */
#include "nvs_store.h"
#include "board_jc4880p443c.h"

#include <string.h>
#include <math.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

static const char *TAG = "mb_rtu";

/* Compile-time guard for the NVS blob layout (see the note in modbus_rtu.h):
 * bus[] stride and the offset of the appended bridge block are the whole
 * migration contract, so pin them rather than trusting a comment. */
_Static_assert(sizeof(mb_rtu_bus_cfg_t) == 8, "mb_rtu_bus_cfg_t stride");
_Static_assert(offsetof(mb_rtu_cfg_t, gw_enabled) == 16, "gw block must follow bus[]");
_Static_assert(sizeof(mb_rtu_cfg_t) == 24, "mb_rtu_cfg_t layout");

#define RX_BUF          512
#define TX_BUF          256

/* Deye SG04LP3 holding registers (same map as the TCP path). */
#define DEYE_SOC_REG    588
#define DEYE_PWR_REG    590     /* S16, +discharge / -charge */

/* Interval of the regular Deye battery poll on a MASTER bus. */
#define DEYE_POLL_MS    2000

/* Max age of the grid reading we still feed the Deye as a REAL value through the
 * Eastron emulation. Within this -> report real grid power (zero-export works,
 * tolerant of a congested multi-device poll cycle). Older (poll stalled / no
 * grid meter) -> report a balanced 0 instead (see do_slave): keeps the meter
 * alive for the Deye while giving it no frozen value to chase. */
#define MB_GRID_MAX_AGE_MS  12000

/* Fixed per-bus UART + pins (bus 0 = A, bus 1 = B). */
static const struct { int uart, tx, rx; } BUS_HW[MB_RTU_BUSES] = {
    { BOARD_RS485_A_UART, BOARD_RS485_A_TX, BOARD_RS485_A_RX },
    { BOARD_RS485_B_UART, BOARD_RS485_B_TX, BOARD_RS485_B_RX },
};

static mb_rtu_cfg_t          s_cfg;
static mb_rtu_status_t       s_st;
static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int          s_grid_sp;      /* grid setpoint (W) for zero-export trick */
static volatile bool         s_selftest_req  = false;
static mb_rtu_selftest_result_t s_selftest_result = { .state = MB_RTU_SELFTEST_IDLE, .latency_ms = -1 };

/* On-demand Deye register access for the /deye web page. The HTTP handler fills
 * a request and the Deye-master bus task serves it between its regular polls
 * (so the single RS485 UART is never touched by two tasks at once). A mutex
 * serialises concurrent HTTP callers. */
#define DEYE_REQ_MAX 64
typedef struct {
    volatile bool pending;
    volatile bool done;
    bool          is_write;     /* false = FC03 read, true = FC06 write */
    uint16_t      addr;
    uint16_t      count;        /* read count (<= DEYE_REQ_MAX); 1 for write */
    uint16_t      wval;         /* value to write */
    uint16_t      result[DEYE_REQ_MAX];
    int           rc;           /* 0 = ok, <0 = error */
} deye_req_t;
static deye_req_t        s_deye_req;
static SemaphoreHandle_t s_deye_req_mtx;

/* Bridge slot, one per bus: modbus_gw.c parks a raw RTU frame here and the bus
 * task runs it between its own polls (same "never touch the UART from two
 * tasks" rule as s_deye_req, but per-bus so two masters can serve in
 * parallel). `lock` serialises the TCP clients, `done` wakes the waiter. */
typedef struct {
    volatile bool     pending;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    uint8_t           req[MB_RTU_ADU_MAX];   /* +2 bytes room for the CRC   */
    int               req_len;               /* without CRC                 */
    uint8_t           resp[MB_RTU_ADU_MAX];
    int               rc;                    /* rtu_raw(): len > 0, or < 0  */
    uint32_t          timeout_ms;
} rtu_txn_t;
static rtu_txn_t s_txn[MB_RTU_BUSES];

/* When the regular Deye poll is next due, per bus (0 = immediately). */
static int64_t s_next_poll_us[MB_RTU_BUSES];

/* ----------------------------- helpers ------------------------------- */

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    return c;
}

/* Fill in defaults for unset/out-of-range fields. Also the migration path: an
 * NVS record written before the bridge existed is 16 bytes, so every gw_* field
 * reads 0 here and lands on its default (bridge off). */
static void clamp_cfg(mb_rtu_cfg_t *c)
{
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        if (c->bus[i].baud == 0)     c->bus[i].baud = 9600;
        if (c->bus[i].slave_id == 0) c->bus[i].slave_id = 1;
        if (c->bus[i].role > MB_RTU_SLAVE) c->bus[i].role = MB_RTU_MASTER;
    }
    if (c->gw_port == 0)       c->gw_port = MB_GW_DEFAULT_PORT;
    if (c->gw_timeout_ms == 0) c->gw_timeout_ms = MB_GW_DEFAULT_TIMEOUT_MS;
    if (c->gw_timeout_ms < 100)   c->gw_timeout_ms = 100;
    if (c->gw_timeout_ms > 5000)  c->gw_timeout_ms = 5000;
    if (c->gw_max_clients == 0)                    c->gw_max_clients = MB_GW_DEFAULT_CLIENTS;
    if (c->gw_max_clients > MB_GW_MAX_CLIENTS)     c->gw_max_clients = MB_GW_MAX_CLIENTS;
    /* No bus picked but the bridge is on -> serve every master bus, so simply
     * switching it on does something useful instead of answering 0x0A. */
    if (c->gw_enabled && c->gw_bus_mask == 0)
        c->gw_bus_mask = (1u << MB_RTU_BUSES) - 1u;
}

static void load_cfg(void)
{
    mb_rtu_cfg_t c;
    memset(&c, 0, sizeof(c));
    if (nvs_store_get_mb_rtu(&c, sizeof(c)) != ESP_OK) {
        /* Defaults: bus A = Eastron slave, bus B = Deye master, both off. */
        c.bus[0].role = MB_RTU_SLAVE;
        c.bus[1].role = MB_RTU_MASTER;
    }
    clamp_cfg(&c);
    portENTER_CRITICAL(&s_mux);
    s_cfg = c;
    portEXIT_CRITICAL(&s_mux);
}

static void uart_setup(int port, int tx, int rx, uint32_t baud)
{
    uart_config_t cfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,   /* 8N1 (Eastron / Deye default) */
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (!uart_is_driver_installed(port))
        uart_driver_install(port, RX_BUF, TX_BUF, 0, NULL, 0);
    uart_param_config(port, &cfg);
    uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/* --------------------- Eastron SDM630 emulation ---------------------- */

/* IEEE-754 value for an SDM630 input-register pair starting at `base` (even
 * address). SDM630 sign: + = import (from grid), - = export -- same as
 * modbus_tcp grid_w. */
static float sdm630_value(uint16_t base, float grid_w)
{
    float p_phase = grid_w / 3.0f;
    switch (base) {
    case 0x0000: case 0x0002: case 0x0004: return 230.0f;                 /* V L1..L3 */
    case 0x0006: case 0x0008: case 0x000A: return fabsf(p_phase) / 230.0f;/* I L1..L3 */
    case 0x000C: case 0x000E: case 0x0010: return p_phase;                /* P L1..L3 */
    case 0x0034: return grid_w;                                           /* total P  */
    case 0x0046: return 50.0f;                                            /* frequency*/
    default:     return 0.0f;
    }
}

/* Build an FC03/FC04 read response into `out`, return its length (0 = drop). */
static int sdm630_response(const uint8_t *req, uint8_t *out, float grid_w)
{
    uint8_t  slave = req[0], fc = req[1];
    uint16_t addr  = (uint16_t)((req[2] << 8) | req[3]);
    uint16_t cnt   = (uint16_t)((req[4] << 8) | req[5]);
    if (cnt == 0 || cnt > 125) return 0;

    out[0] = slave; out[1] = fc; out[2] = (uint8_t)(cnt * 2);
    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t reg  = addr + i;
        uint16_t base = reg & ~1u;
        float    f    = sdm630_value(base, grid_w);
        uint32_t u;   memcpy(&u, &f, sizeof(u));
        uint16_t word = (reg & 1u) ? (uint16_t)(u & 0xFFFF) : (uint16_t)(u >> 16);
        out[3 + i * 2]     = (uint8_t)(word >> 8);
        out[3 + i * 2 + 1] = (uint8_t)(word & 0xFF);
    }
    int n = 3 + cnt * 2;
    uint16_t c = crc16(out, n);
    out[n]     = (uint8_t)(c & 0xFF);
    out[n + 1] = (uint8_t)(c >> 8);
    return n + 2;
}

static void do_slave(int idx, int port, const mb_rtu_bus_cfg_t *c)
{
    uint8_t req[16];
    int n = uart_read_bytes(port, req, 8, pdMS_TO_TICKS(150));
    if (n != 8) { if (n > 0) uart_flush_input(port); return; }

    uint16_t rc = crc16(req, 6);
    if (req[0] != c->slave_id || (req[1] != 3 && req[1] != 4) ||
        ((req[6] | (req[7] << 8)) != rc)) {
        uart_flush_input(port);
        return;
    }

    /* Zero-export trick: report (real grid - setpoint) so the Deye drives the
     * real grid point to the setpoint instead of to 0. CRITICAL: only do this
     * with a FRESH grid reading. A stale/frozen value here is a dead sensor in
     * the inverter's control loop -- it once drove a 15 kW export runaway. If
     * the reading is stale, stay silent: the Deye sees a meter timeout and
     * falls back to its own CT. */
    /* When fresh, report the real grid power; when stale (poll slow/stalled),
     * report a BALANCED 0 -- but ALWAYS answer. Going silent makes the Deye flag
     * "meter lost / not connected". A frozen real value would let it chase a
     * dead sensor (the 15 kW runaway). 0 keeps the meter alive AND gives the
     * Deye no error to chase, so it just holds until fresh data returns. */
    static bool s_grid_was_fresh = true;
    float grid_w = 0.0f;
    bool fresh = modbus_tcp_grid_w_fresh(&grid_w, MB_GRID_MAX_AGE_MS);
    if (fresh != s_grid_was_fresh) {
        ESP_LOGW(TAG, "grid reading %s -> meter reports %s",
                 fresh ? "fresh again" : "STALE",
                 fresh ? "real grid power" : "0 W (balanced; Deye holds)");
        s_grid_was_fresh = fresh;
    }

    float served = fresh ? (grid_w - (float)s_grid_sp) : 0.0f;
    uint8_t resp[260];
    int rn = sdm630_response(req, resp, served);
    if (rn > 0) {
        uart_write_bytes(port, resp, rn);
        portENTER_CRITICAL(&s_mux);
        s_st.bus[idx].polls++; s_st.bus[idx].a = served;
        portEXIT_CRITICAL(&s_mux);
    }
}

/* ----------------------- Deye RTU master ----------------------------- */

static int rtu_read(int port, uint8_t slave, uint16_t addr, uint16_t cnt, uint16_t *out)
{
    uint8_t req[8] = { slave, 0x03,
                       (uint8_t)(addr >> 8), (uint8_t)addr,
                       (uint8_t)(cnt >> 8),  (uint8_t)cnt };
    uint16_t c = crc16(req, 6);
    req[6] = (uint8_t)(c & 0xFF); req[7] = (uint8_t)(c >> 8);

    uart_flush_input(port);
    uart_write_bytes(port, req, 8);

    int explen = 3 + cnt * 2 + 2;
    uint8_t resp[260];
    if (explen > (int)sizeof(resp)) return -1;
    int got = uart_read_bytes(port, resp, explen, pdMS_TO_TICKS(400));
    if (got != explen) return -1;
    if (resp[0] != slave || resp[1] != 0x03) return -2;
    if ((resp[explen - 2] | (resp[explen - 1] << 8)) != crc16(resp, explen - 2)) return -3;
    for (int i = 0; i < cnt; i++)
        out[i] = (uint16_t)((resp[3 + i * 2] << 8) | resp[3 + i * 2 + 1]);
    return 0;
}

/* FC16: write a single holding register (quantity = 1). The Deye accepts FC16
 * for setting registers (FC06 is not honoured). Returns 0 on OK (response
 * header verified). */
static int rtu_write(int port, uint8_t slave, uint16_t addr, uint16_t val)
{
    uint8_t req[11] = { slave, 0x10,
                        (uint8_t)(addr >> 8), (uint8_t)addr,
                        0x00, 0x01,            /* quantity = 1 register   */
                        0x02,                  /* byte count = 2          */
                        (uint8_t)(val >> 8), (uint8_t)val };
    uint16_t c = crc16(req, 9);
    req[9]  = (uint8_t)(c & 0xFF); req[10] = (uint8_t)(c >> 8);

    uart_flush_input(port);
    uart_write_bytes(port, req, 11);

    /* FC16 response: [id][0x10][addr_hi][addr_lo][qty_hi][qty_lo][crc_lo][crc_hi] */
    uint8_t resp[8];
    int got = uart_read_bytes(port, resp, 8, pdMS_TO_TICKS(400));
    if (got != 8) return -1;
    if (resp[0] != slave || resp[1] != 0x10) return -2;
    if ((resp[6] | (resp[7] << 8)) != crc16(resp, 6)) return -3;
    return 0;
}

/* Serve a pending on-demand /deye request on this master bus, if any. */
static void serve_deye_req(int port, uint8_t slave)
{
    if (!s_deye_req.pending) return;
    if (s_deye_req.is_write) {
        s_deye_req.rc = rtu_write(port, slave, s_deye_req.addr, s_deye_req.wval);
    } else {
        uint16_t cnt = s_deye_req.count;
        if (cnt == 0 || cnt > DEYE_REQ_MAX) { s_deye_req.rc = -10; }
        else s_deye_req.rc = rtu_read(port, slave, s_deye_req.addr, cnt, s_deye_req.result);
    }
    s_deye_req.pending = false;
    s_deye_req.done    = true;
}

/* ------------------- TCP <-> RTU bridge back-end ---------------------- */

/* Read exactly n bytes, bounded by an absolute deadline (esp_timer us). */
static int rtu_read_exact(int port, uint8_t *dst, int n, int64_t deadline)
{
    int got = 0;
    while (got < n) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) return -1;
        int r = uart_read_bytes(port, dst + got, n - got,
                                pdMS_TO_TICKS((uint32_t)(left / 1000) + 1));
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

/* Run one arbitrary RTU request and return the response ADU without its CRC.
 *
 * RTU has no length prefix, so the response size has to be derived from the
 * function code -- that is the whole reason this cannot just be a byte pipe:
 *   FC 01/02/03/04/23 -> [id][fc][bytecount][data..][crc]  (length in byte 2)
 *   FC 05/06/15/16    -> [id][fc][addr][value|qty][crc]    (always 8 bytes)
 *   fc | 0x80         -> [id][fc][exception][crc]          (always 5 bytes)
 * An exception response is a VALID answer and is passed back to the TCP client
 * verbatim -- the inverter refusing a register is the client's business.
 *
 * `req` and `resp` must BOTH be MB_RTU_ADU_MAX bytes -- spelled as [static ...]
 * so the compiler rejects a smaller buffer: the CRC is appended into `req` in
 * place and the answer is received straight into `resp`. Scratch copies would
 * cost 512 B of stack on the bus task, which also runs the Deye poll and the
 * Eastron emulation -- not worth it for two memcpys. */
static int rtu_raw(int port, uint8_t req[static MB_RTU_ADU_MAX], int req_len,
                   uint8_t resp[static MB_RTU_ADU_MAX], uint32_t tmo_ms)
{
    if (req_len < 2 || req_len + 2 > MB_RTU_ADU_MAX) return -10;

    uint16_t c = crc16(req, req_len);
    req[req_len]     = (uint8_t)(c & 0xFF);
    req[req_len + 1] = (uint8_t)(c >> 8);

    uart_flush_input(port);
    if (uart_write_bytes(port, req, req_len + 2) != req_len + 2) return -11;

    int64_t deadline = esp_timer_get_time() + (int64_t)tmo_ms * 1000;
    int     got;

    if (rtu_read_exact(port, resp, 2, deadline) < 0) return -12;  /* timeout */
    got = 2;

    int body;                       /* bytes still outstanding after the header */
    if (resp[1] & 0x80) {
        body = 3;                                   /* exception code + CRC     */
    } else switch (resp[1]) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x17:
        if (rtu_read_exact(port, resp + 2, 1, deadline) < 0) return -12;
        got  = 3;
        body = resp[2] + 2;                         /* data + CRC               */
        break;
    case 0x05: case 0x06: case 0x0F: case 0x10:
        body = 6;                                   /* addr + qty/value + CRC   */
        break;
    default:
        return -13;                                 /* unframeable function     */
    }
    if (got + body > MB_RTU_ADU_MAX) return -14;
    if (rtu_read_exact(port, resp + got, body, deadline) < 0) return -12;
    got += body;

    if ((resp[got - 2] | (resp[got - 1] << 8)) != crc16(resp, got - 2)) return -15;
    if (resp[0] != req[0]) return -16;              /* answer from another id   */

    return got - 2;                                 /* strip CRC                */
}

/* Serve a parked bridge request on this bus, if one is waiting. */
static void serve_txn(int idx, int port)
{
    rtu_txn_t *t = &s_txn[idx];
    t->rc = rtu_raw(port, t->req, t->req_len, t->resp, t->timeout_ms);
    t->pending = false;
    xSemaphoreGive(t->done);
}

static void do_master(int idx, int port, const mb_rtu_bus_cfg_t *c)
{
    /* Serve an on-demand /deye read/write first if one is queued (skip the
     * regular battery read this cycle; it resumes next loop). */
    if (s_deye_req.pending) { serve_deye_req(port, c->slave_id); return; }

    /* Then a parked TCP-bridge request. Both jump the poll cadence so a
     * Modbus-TCP client sees gateway-typical latency, not multi-second waits. */
    if (s_txn[idx].pending) { serve_txn(idx, port); return; }

    /* The poll cadence is a DEADLINE, not "once per loop": serving a request
     * returns from here, so a loop-counted poll would fire again immediately
     * afterwards and a 5 Hz bridge client would multiply the Deye's own RS485
     * traffic tenfold -- on the bus that carries the control path. */
    if (esp_timer_get_time() >= s_next_poll_us[idx]) {
        s_next_poll_us[idx] = esp_timer_get_time() + DEYE_POLL_MS * 1000;

        /* 586..590 covers SoC(588) + battery power(590) in one short read. */
        uint16_t r[5];
        int rc = rtu_read(port, c->slave_id, 586, 5, r);
        if (rc == 0) {
            float soc = (float)r[DEYE_SOC_REG - 586];
            float w   = (float)(int16_t)r[DEYE_PWR_REG - 586];   /* +discharge */
            modbus_tcp_set_rtu_deye(w, soc, true);
            portENTER_CRITICAL(&s_mux);
            s_st.bus[idx].online = true; s_st.bus[idx].polls++;
            s_st.bus[idx].a = w; s_st.bus[idx].b = soc;
            portEXIT_CRITICAL(&s_mux);
        } else {
            modbus_tcp_set_rtu_deye(0, 0, false);
            portENTER_CRITICAL(&s_mux);
            s_st.bus[idx].online = false; s_st.bus[idx].errs++;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGW(TAG, "bus%d Deye RTU read failed (%d)", idx, rc);
        }
    }

    /* Break the poll interval early for a self-test or a queued /deye or
     * TCP-bridge request. A bridged bus is checked every 20 ms so a Modbus-TCP
     * client polling once a second does not spend most of its round-trip
     * waiting for this loop to notice; without the bridge there is nothing to
     * notice quickly, so stay at 100 ms and keep this priority-5 task (above
     * LVGL) at 10 wakeups/s instead of 50. */
    const int step = modbus_rtu_bus_can_gateway(idx) ? 20 : 100;
    for (int i = 0; i < DEYE_POLL_MS / step && !s_selftest_req &&
                    !s_deye_req.pending && !s_txn[idx].pending; i++)
        vTaskDelay(pdMS_TO_TICKS(step));
}

/* ----------------------- Self-test ------------------------------------ */

/* Bus B (master UART, port) sends a Modbus FC03 request to bus A's slave_id.
 * Bus A's slave task, running normally, picks it up and responds.
 * Called from bus 1's task with bus 1's UART port. */
static void do_selftest(int port, uint8_t slave_id)
{
    mb_rtu_selftest_result_t res = { .state = MB_RTU_SELFTEST_FAIL, .latency_ms = -1 };

    /* FC03: read 2 registers at address 0 (SDM630 Phase-1 voltage). */
    uint8_t req[8] = { slave_id, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
    uint16_t c = crc16(req, 6);
    req[6] = (uint8_t)(c & 0xFF);
    req[7] = (uint8_t)(c >> 8);

    uart_flush_input(port);
    int64_t t0 = esp_timer_get_time();
    uart_write_bytes(port, req, 8);

    /* Expected response: [id][0x03][0x04][b0..b3][b4..b7][crclo][crchi] = 9 bytes */
    uint8_t resp[9];
    int got = uart_read_bytes(port, resp, sizeof(resp), pdMS_TO_TICKS(500));
    res.latency_ms = (int32_t)((esp_timer_get_time() - t0) / 1000);

    if (got != 9) {
        snprintf(res.error, sizeof(res.error), "Timeout: %d/9 Bytes empfangen", got);
    } else {
        uint16_t calc = crc16(resp, 7);
        uint16_t recv = (uint16_t)(resp[7] | (resp[8] << 8));
        if (calc != recv) {
            snprintf(res.error, sizeof(res.error),
                     "CRC-Fehler: erw.=%04X got=%04X", calc, recv);
        } else if (resp[0] != slave_id || resp[1] != 0x03 || resp[2] != 0x04) {
            snprintf(res.error, sizeof(res.error),
                     "Ungueltige Antwort: id=%02X fc=%02X cnt=%02X",
                     resp[0], resp[1], resp[2]);
        } else {
            res.state = MB_RTU_SELFTEST_PASS;
        }
    }

    ESP_LOGI(TAG, "selftest: %s (%ldms)%s%s",
             res.state == MB_RTU_SELFTEST_PASS ? "PASS" : "FAIL",
             (long)res.latency_ms,
             res.state != MB_RTU_SELFTEST_PASS ? " - " : "",
             res.state != MB_RTU_SELFTEST_PASS ? res.error : "");

    portENTER_CRITICAL(&s_mux);
    s_selftest_result = res;
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------- per-bus task ------------------------------ */

static void bus_task(void *arg)
{
    int idx  = (int)(intptr_t)arg;
    int port = BUS_HW[idx].uart, tx = BUS_HW[idx].tx, rx = BUS_HW[idx].rx;
    uint32_t applied_baud = 0;
    bool     installed    = false;

    for (;;) {
        mb_rtu_cfg_t all; modbus_rtu_get_cfg(&all);
        mb_rtu_bus_cfg_t c = all.bus[idx];

        if (!installed || c.baud != applied_baud) {
            uart_setup(port, tx, rx, c.baud ? c.baud : 9600);
            uart_flush_input(port);
            applied_baud = c.baud;
            installed = true;
        }

        portENTER_CRITICAL(&s_mux);
        s_st.bus[idx].running = c.enabled;
        s_st.bus[idx].role    = c.role;
        portEXIT_CRITICAL(&s_mux);

        /* Self-test runs on the master bus (idx 1); slave (idx 0) responds normally. */
        if (idx == 1 && s_selftest_req) {
            s_selftest_req = false;
            mb_rtu_cfg_t tmp; modbus_rtu_get_cfg(&tmp);
            do_selftest(port, tmp.bus[0].slave_id);
            continue;
        }

        if (!c.enabled) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        if (c.role == MB_RTU_SLAVE) do_slave(idx, port, &c);
        else                        do_master(idx, port, &c);
    }
}

/* ----------------------------- public -------------------------------- */

const char *modbus_rtu_role_name(uint8_t role)
{
    return role == MB_RTU_SLAVE ? "Slave (Eastron)" : "Master (Deye)";
}

int modbus_rtu_get_grid_setpoint(void) { return s_grid_sp; }

void modbus_rtu_set_grid_setpoint(int w)
{
    s_grid_sp = w;
    nvs_store_set_grid_sp(w);
}

/* On-demand Deye access for the /deye web page. Queues a request that the
 * Deye-master bus task serves, then waits for the result. Returns 0 on success,
 * a negative Modbus/transport error, or a negative timeout/availability code. */
static int deye_req_run(bool is_write, uint16_t addr, uint16_t count,
                        uint16_t wval, uint16_t *out)
{
    if (!s_deye_req_mtx) return -101;          /* not started */
    if (xSemaphoreTake(s_deye_req_mtx, pdMS_TO_TICKS(3000)) != pdTRUE) return -102;

    s_deye_req.is_write = is_write;
    s_deye_req.addr     = addr;
    s_deye_req.count    = count;
    s_deye_req.wval     = wval;
    s_deye_req.rc       = -1;
    s_deye_req.done     = false;
    s_deye_req.pending  = true;                /* set LAST -> bus task picks it up */

    int rc = -103;                             /* timeout (no master bus running?) */
    for (int i = 0; i < 60; i++) {             /* up to ~3 s */
        if (s_deye_req.done) {
            rc = s_deye_req.rc;
            if (rc == 0 && !is_write && out)
                for (int k = 0; k < count; k++) out[k] = s_deye_req.result[k];
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_deye_req.pending = false;
    xSemaphoreGive(s_deye_req_mtx);
    return rc;
}

int modbus_rtu_deye_read(uint16_t addr, uint16_t count, uint16_t *out)
{
    if (count == 0 || count > DEYE_REQ_MAX || !out) return -100;
    return deye_req_run(false, addr, count, 0, out);
}

int modbus_rtu_deye_write(uint16_t addr, uint16_t val)
{
    return deye_req_run(true, addr, 1, val, NULL);
}

/* ---------------- TCP <-> RTU bridge back-end -------------------------- */

/* The eligibility rule, in ONE place -- it is the safety invariant that keeps
 * injected traffic off a SLAVE bus, so it must not exist as two copies that can
 * drift apart. Caller holds s_mux; `bus` is already range-checked. */
static inline bool bus_can_gw_nolock(int bus)
{
    return s_cfg.gw_enabled && (s_cfg.gw_bus_mask & (1u << bus)) &&
           s_cfg.bus[bus].enabled && s_cfg.bus[bus].role == MB_RTU_MASTER;
}

bool modbus_rtu_bus_can_gateway(int bus)
{
    if (bus < 0 || bus >= MB_RTU_BUSES) return false;
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = bus_can_gw_nolock(bus);
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

int modbus_rtu_gw_route(uint8_t unit)
{
    int hit = -1, only = -1, n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        if (!bus_can_gw_nolock(i)) continue;
        n++; only = i;
        if (s_cfg.bus[i].slave_id == unit && hit < 0) hit = i;
    }
    portEXIT_CRITICAL(&s_mux);
    /* Exact slave-id match wins (lets both buses be addressed at once). With a
     * single bridge bus, everything else falls through to it so other slaves on
     * that RS485 segment stay reachable. */
    if (hit >= 0) return hit;
    return (n == 1) ? only : -1;
}

int modbus_rtu_txn(int bus, const uint8_t *req, int req_len,
                   uint8_t *resp, int resp_max, uint32_t timeout_ms)
{
    if (bus < 0 || bus >= MB_RTU_BUSES)          return -100;
    if (!req || !resp)                           return -101;
    if (req_len < 2 || req_len + 2 > MB_RTU_ADU_MAX) return -102;
    if (!modbus_rtu_bus_can_gateway(bus))        return -103;

    rtu_txn_t *t = &s_txn[bus];
    if (!t->lock || !t->done)                    return -104;

    /* Serialise the TCP clients: one RS485 exchange at a time per bus. */
    if (xSemaphoreTake(t->lock, pdMS_TO_TICKS(timeout_ms + 2000)) != pdTRUE)
        return -105;

    xSemaphoreTake(t->done, 0);      /* drop a stale signal from a past timeout */

    memcpy(t->req, req, req_len);
    t->req_len    = req_len;
    t->rc         = -106;
    t->timeout_ms = timeout_ms;
    t->pending    = true;            /* set LAST -- the bus task picks it up    */

    int rc;
    /* Budget: up to 20 ms for the bus task to notice + the RTU exchange itself,
     * plus slack for a poll/write it may already be in the middle of. */
    if (xSemaphoreTake(t->done, pdMS_TO_TICKS(timeout_ms + 2000)) == pdTRUE) {
        rc = t->rc;                  /* rtu_raw(): response length, or < 0      */
        if (rc > 0) {
            if (rc > resp_max) rc = -107;
            else memcpy(resp, t->resp, rc);
        }
    } else {
        /* The bus task may be mid-exchange and about to write t->resp. Give it
         * a grace period before releasing the lock, otherwise its late result
         * would land in the NEXT caller's buffers. */
        if (xSemaphoreTake(t->done, pdMS_TO_TICKS(3000)) != pdTRUE)
            t->pending = false;
        rc = -108;                   /* bus task stalled or bus switched off    */
    }
    xSemaphoreGive(t->lock);
    return rc;
}

esp_err_t modbus_rtu_start(void)
{
    s_grid_sp = nvs_store_get_grid_sp();
    if (!s_deye_req_mtx) s_deye_req_mtx = xSemaphoreCreateMutex();
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        if (!s_txn[i].lock) s_txn[i].lock = xSemaphoreCreateMutex();
        if (!s_txn[i].done) s_txn[i].done = xSemaphoreCreateBinary();
        if (!s_txn[i].lock || !s_txn[i].done) return ESP_ERR_NO_MEM;
    }
    load_cfg();
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        char name[12];
        snprintf(name, sizeof(name), "rtu_bus%d", i);
        if (xTaskCreate(bus_task, name, 4096, (void *)(intptr_t)i, 5, NULL) != pdPASS)
            return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Modbus-RTU started (bus0=UART%d, bus1=UART%d)",
             BUS_HW[0].uart, BUS_HW[1].uart);
    return ESP_OK;
}

void modbus_rtu_get_status(mb_rtu_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void modbus_rtu_get_cfg(mb_rtu_cfg_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t modbus_rtu_set_cfg(const mb_rtu_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    mb_rtu_cfg_t c = *cfg;
    clamp_cfg(&c);
    esp_err_t e = nvs_store_set_mb_rtu(&c, sizeof(c));
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_cfg = c;
        portEXIT_CRITICAL(&s_mux);
        /* clear stale Deye value; an enabled master re-populates within 2 s */
        modbus_tcp_set_rtu_deye(0, 0, false);
    }
    return e;
}

void modbus_rtu_selftest_start(void)
{
    portENTER_CRITICAL(&s_mux);
    s_selftest_result.state      = MB_RTU_SELFTEST_PENDING;
    s_selftest_result.latency_ms = -1;
    s_selftest_result.error[0]   = '\0';
    portEXIT_CRITICAL(&s_mux);
    s_selftest_req = true;   /* picked up by bus 1's task on its next loop */
}

mb_rtu_selftest_result_t modbus_rtu_selftest_result(void)
{
    mb_rtu_selftest_result_t r;
    portENTER_CRITICAL(&s_mux);
    r = s_selftest_result;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

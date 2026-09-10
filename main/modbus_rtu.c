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

/* MB_GRID_MAX_AGE_MS (the age at which the grid reading stops counting as real)
 * lives in modbus_tcp.h -- the same bound guards the SLS export protection. */

/* Nominal values the emulation synthesises around: the Deye only uses P, but a
 * plausible U/I keeps meter-detection happy. */
#define SDM_NOMINAL_V   230.0f
/* Hard clamp on anything we hand the inverter. A manipulated phase is allowed to
 * be extreme (that is the point), but never inf/NaN or a value that would make
 * the Deye's own limit logic wrap. */
#define MB_SERVED_CLAMP_W  100000.0f

/* Fixed per-bus UART + pins (bus 0 = A, bus 1 = B). */
static const struct { int uart, tx, rx; } BUS_HW[MB_RTU_BUSES] = {
    { BOARD_RS485_A_UART, BOARD_RS485_A_TX, BOARD_RS485_A_RX },
    { BOARD_RS485_B_UART, BOARD_RS485_B_TX, BOARD_RS485_B_RX },
};

static mb_rtu_cfg_t          s_cfg;
static mb_rtu_status_t       s_st;
static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int          s_grid_sp;      /* grid setpoint (W) for zero-export trick */

/* Per-phase manipulation of the values served to the Deye (under s_mux), plus
 * how often the Deye has actually asked -- the /meter page shows both. */
static mb_manip_cfg_t        s_manip;         /* master switch defaults to OFF */
static uint32_t              s_requests;      /* SDM630 requests answered       */
static uint32_t              s_request_ms;    /* ms of the last answered request */
static uint32_t              s_manip_ms;      /* when manipulation was switched on */
static bool                  s_slave_quiet;   /* bridge over -> not answering   */
static uint32_t              s_stale_ms;      /* how long the value is stale    */
static volatile bool         s_selftest_req  = false;
static mb_rtu_selftest_result_t s_selftest_result = { .state = MB_RTU_SELFTEST_IDLE, .latency_ms = -1 };

/* The inverter's own live measurements (see mb_deye_live_t). Written only by
 * the master bus task, read by the web handler -- copied under s_mux so the
 * page never sees half of one round next to half of the previous one. */
static mb_deye_live_t        s_live;
static uint32_t              s_live_ms;      /* ms of the last successful round */

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
    /* gw_max_clients has NO control in the "Mod RTU" tab, and rtu_save_cb writes
     * the whole struct back -- so the stored byte is never a user choice, only a
     * snapshot of whatever the compiled default was when the bridge was last
     * switched on. Clamping it would pin an old default in NVS forever (raising
     * MB_GW_DEFAULT_CLIENTS would silently do nothing on every device that has
     * ever saved). Derive it instead. If this ever becomes user-editable, delete
     * this line and restore the 0 -> default / > MAX -> MAX clamp. */
    c->gw_max_clients = MB_GW_DEFAULT_CLIENTS;
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

/* Reject anything that could turn into inf/NaN inside compute_served(). An
 * unknown mode degrades to pass-through rather than to undefined behaviour. */
static void clamp_manip(mb_manip_cfg_t *m)
{
    m->enabled = m->enabled ? 1 : 0;
    for (int k = 0; k < 3; k++) {
        if (m->ph[k].mode >= MB_PH_MODE_COUNT) m->ph[k].mode = MB_PH_OFF;
        if (!isfinite(m->ph[k].value))         m->ph[k].value = 0.0f;
        if (m->ph[k].value >  MB_SERVED_CLAMP_W) m->ph[k].value =  MB_SERVED_CLAMP_W;
        if (m->ph[k].value < -MB_SERVED_CLAMP_W) m->ph[k].value = -MB_SERVED_CLAMP_W;
    }
}

static void load_manip(void)
{
    mb_manip_cfg_t m;
    memset(&m, 0, sizeof(m));
    /* Absent blob -> all zeroes = master switch off, every phase pass-through. */
    nvs_store_get_mb_manip(&m, sizeof(m));
    clamp_manip(&m);
    /* Switched OFF on every start. Manipulation exists to try something out
     * while somebody watches; it lives only in this device, so resuming it
     * after an unattended restart (power cut, OTA, crash) would mean quietly
     * feeding the inverter wrong numbers with nobody around. The values are
     * kept, only the master switch drops -- turning it back on is one tap. */
    if (m.enabled) {
        ESP_LOGW(TAG, "phase manipulation was ON before the restart "
                      "(L1=%s/%.0f L2=%s/%.0f L3=%s/%.0f) -- switching it OFF",
                 modbus_rtu_phase_mode_name(m.ph[0].mode), m.ph[0].value,
                 modbus_rtu_phase_mode_name(m.ph[1].mode), m.ph[1].value,
                 modbus_rtu_phase_mode_name(m.ph[2].mode), m.ph[2].value);
        m.enabled = 0;
        nvs_store_set_mb_manip(&m, sizeof(m));
    }
    portENTER_CRITICAL(&s_mux);
    s_manip = m;
    s_manip_ms = (uint32_t)(esp_timer_get_time() / 1000);
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
 * modbus_tcp grid_w. `p` carries the three phase powers already served (real,
 * setpoint-shifted and manipulated); `total` is their sum. */
static float sdm630_value(uint16_t base, const float p[3], float total)
{
    switch (base) {
    case 0x0000: case 0x0002: case 0x0004: return SDM_NOMINAL_V;      /* V L1..L3 */
    case 0x0006: return fabsf(p[0]) / SDM_NOMINAL_V;                  /* I L1     */
    case 0x0008: return fabsf(p[1]) / SDM_NOMINAL_V;                  /* I L2     */
    case 0x000A: return fabsf(p[2]) / SDM_NOMINAL_V;                  /* I L3     */
    case 0x000C: return p[0];                                         /* P L1     */
    case 0x000E: return p[1];                                         /* P L2     */
    case 0x0010: return p[2];                                         /* P L3     */
    case 0x0034: return total;                                        /* total P  */
    case 0x0046: return 50.0f;                                        /* frequency*/
    default:     return 0.0f;
    }
}

/* Build an FC03/FC04 read response into `out`, return its length (0 = drop). */
static int sdm630_response(const uint8_t *req, uint8_t *out, const float p[3], float total)
{
    uint8_t  slave = req[0], fc = req[1];
    uint16_t addr  = (uint16_t)((req[2] << 8) | req[3]);
    uint16_t cnt   = (uint16_t)((req[4] << 8) | req[5]);
    if (cnt == 0 || cnt > 125) return 0;

    out[0] = slave; out[1] = fc; out[2] = (uint8_t)(cnt * 2);
    for (uint16_t i = 0; i < cnt; i++) {
        uint16_t reg  = addr + i;
        uint16_t base = reg & ~1u;
        float    f    = sdm630_value(base, p, total);
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

/* Turn the (fresh) real grid reading into the three phase values the Deye is
 * told about.
 *
 * real phase  ->  minus setpoint/3  ->  per-phase manipulation  ->  served
 *
 * When the meter delivers no per-phase data (or is not an Eltako) the total is
 * split evenly, which is exactly what this emulation did before -- so an
 * unmanipulated three-phase-blind setup behaves bit-for-bit as it used to.
 * When the reading is STALE every phase is served as 0 (see do_slave): the
 * meter stays alive for the Deye but gives it nothing to chase -- and only for
 * slave_hold_s seconds, after which do_slave() stops answering entirely.
 *
 * Deliberately SIDE-EFFECT FREE, so the /meter page can call it to show the
 * chain even while no Deye is polling us -- otherwise the page would read all
 * zeroes on a setup that has no RTU slave bus running. Optional outputs may be
 * NULL. */
static void compute_served(bool fresh, float grid_w, bool *per_phase_out,
                           float real_out[3], float p_out[3], float *total_out)
{
    float real[3];
    bool per_phase = fresh && modbus_tcp_grid_phases_fresh(real, MB_GRID_MAX_AGE_MS);
    if (!per_phase) real[0] = real[1] = real[2] = grid_w / 3.0f;

    mb_manip_cfg_t m;
    portENTER_CRITICAL(&s_mux);
    m = s_manip;
    portEXIT_CRITICAL(&s_mux);

    float sp_phase = (float)s_grid_sp / 3.0f;
    float total = 0.0f;
    for (int k = 0; k < 3; k++) {
        float v = fresh ? (real[k] - sp_phase) : 0.0f;
        if (fresh && m.enabled) {
            switch (m.ph[k].mode) {
            case MB_PH_OFFSET: v += m.ph[k].value;            break;
            case MB_PH_ABS:    v  = m.ph[k].value;            break;
            case MB_PH_SCALE:  v *= m.ph[k].value / 100.0f;   break;
            default:                                          break;
            }
        }
        if (!isfinite(v)) v = 0.0f;
        if (v >  MB_SERVED_CLAMP_W) v =  MB_SERVED_CLAMP_W;
        if (v < -MB_SERVED_CLAMP_W) v = -MB_SERVED_CLAMP_W;
        if (p_out)    p_out[k]    = v;
        if (real_out) real_out[k] = fresh ? real[k] : 0.0f;
        total += v;
    }
    if (per_phase_out) *per_phase_out = per_phase;
    if (total_out)     *total_out     = total;
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
     * with a FRESH grid reading -- a frozen value here is a dead sensor in the
     * inverter's control loop, it once drove a 15 kW export runaway. What
     * happens instead when it is stale is the bridge below. */
    /* When fresh, report the real grid power. When stale, report a BALANCED 0 --
     * never the frozen real value, which would be a dead sensor in the
     * inverter's control loop (that caused a 15 kW export runaway).
     *
     * But 0 W is a BRIDGE, not a resting state: it keeps the meter alive for the
     * Deye, and the Deye then holds whatever it was doing -- fine for a hiccup,
     * wrong for a ten-minute router reboot at 5 kW discharge, because it holds
     * that 5 kW however the house load moves. So after slave_hold_s seconds we
     * stop answering: the Deye reports a meter failure and falls back to its own
     * CT, i.e. to a real measurement instead of our silence.
     * slave_hold_s = MB_SLAVE_HOLD_FOREVER keeps the old unbounded behaviour. */
    static bool     s_grid_was_fresh = true;
    static uint32_t s_stale_since;              /* ms when freshness was lost */
    static bool     s_quiet_logged;
    float grid_w = 0.0f;
    bool fresh = modbus_tcp_grid_w_fresh(&grid_w, MB_GRID_MAX_AGE_MS);
    uint32_t nowm = (uint32_t)(esp_timer_get_time() / 1000);

    uint8_t hold_cfg;
    portENTER_CRITICAL(&s_mux);
    hold_cfg = s_cfg.slave_hold_s;
    portEXIT_CRITICAL(&s_mux);
    uint16_t hold_s = hold_cfg ? hold_cfg : MB_SLAVE_HOLD_DEFAULT_S;

    if (fresh != s_grid_was_fresh) {
        if (!fresh) { s_stale_since = nowm; s_quiet_logged = false; }
        ESP_LOGW(TAG, "grid reading %s -> meter reports %s",
                 fresh ? "fresh again" : "STALE",
                 fresh ? "real grid power" : "0 W (bridge; Deye holds)");
        s_grid_was_fresh = fresh;
    }

    if (!fresh && hold_cfg != MB_SLAVE_HOLD_FOREVER &&
        (uint32_t)(nowm - s_stale_since) > (uint32_t)hold_s * 1000u) {
        if (!s_quiet_logged) {
            ESP_LOGW(TAG, "grid reading stale for >%u s -- meter emulation going "
                          "SILENT so the Deye falls back to its own CT", (unsigned)hold_s);
            s_quiet_logged = true;
        }
        portENTER_CRITICAL(&s_mux);
        s_slave_quiet = true;
        s_stale_ms    = (uint32_t)(nowm - s_stale_since);
        portEXIT_CRITICAL(&s_mux);
        return;                                 /* no response at all */
    }
    portENTER_CRITICAL(&s_mux);
    s_slave_quiet = false;
    s_stale_ms    = fresh ? 0 : (uint32_t)(nowm - s_stale_since);
    portEXIT_CRITICAL(&s_mux);

    /* Manipulation expires on its own -- checked here, at the moment it would
     * take effect. Cleared in RAM only: an NVS write on this task would delay
     * an answer the Deye is waiting for, and the next start disables it in
     * flash anyway (load_manip). */
    bool manip_expired = false;
    portENTER_CRITICAL(&s_mux);
    if (s_manip.enabled &&
        (uint32_t)(nowm - s_manip_ms) > (uint32_t)MB_MANIP_MAX_S * 1000u) {
        s_manip.enabled = 0;
        manip_expired = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (manip_expired)
        ESP_LOGW(TAG, "phase manipulation ran for %d min -- switched OFF",
                 MB_MANIP_MAX_S / 60);

    float served_p[3], served_total;
    compute_served(fresh, grid_w, NULL, NULL, served_p, &served_total);

    uint8_t resp[260];
    int rn = sdm630_response(req, resp, served_p, served_total);
    if (rn > 0) {
        uart_write_bytes(port, resp, rn);
        portENTER_CRITICAL(&s_mux);
        s_st.bus[idx].polls++; s_st.bus[idx].a = served_total;
        s_requests++;
        s_request_ms = (uint32_t)(esp_timer_get_time() / 1000);
        portEXIT_CRITICAL(&s_mux);
    }
}

/* ----------------------- Deye RTU master ----------------------------- */

/* After a timeout the answer may still be on its way. uart_flush_input()
 * before the NEXT request only drops what has already arrived; a frame that
 * lands a moment later was accepted as the reply to that next request (valid
 * CRC, same id and fc). Wait for the line to be quiet -- 50 ms is ~50 byte
 * times at 9600 baud -- before letting anyone transmit again. */
static void rtu_drain(int port)
{
    uint8_t junk[64];
    for (int i = 0; i < 10; i++) {                       /* at most ~500 ms */
        int n = uart_read_bytes(port, junk, sizeof(junk), pdMS_TO_TICKS(50));
        if (n <= 0) return;
    }
}

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
    if (got != explen) { rtu_drain(port); return -1; }
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
    if (got != 8) { rtu_drain(port); return -1; }
    if (resp[0] != slave || resp[1] != 0x10) return -2;
    if ((resp[6] | (resp[7] << 8)) != crc16(resp, 6)) return -3;
    /* The echo must name OUR register. A late echo of the previous write has
     * a valid CRC and the same id/fc -- without this check it counted as
     * success for a register that was never written. */
    if (memcmp(&resp[2], &req[2], 4) != 0) return -4;
    return 0;
}

/* Serve a pending on-demand /deye request on this master bus, if any. */
static void serve_deye_req(int port, uint8_t slave)
{
    if (!s_deye_req.pending) return;
    /* Work on a copy: the caller may time out and the NEXT caller may already
     * be filling s_deye_req while we are still on the bus. Results go back
     * only if this request is still the one being waited for. */
    bool     is_write = s_deye_req.is_write;
    uint16_t addr = s_deye_req.addr, cnt = s_deye_req.count, wval = s_deye_req.wval;
    uint16_t result[DEYE_REQ_MAX];
    int rc;
    if (is_write) {
        rc = rtu_write(port, slave, addr, wval);
    } else if (cnt == 0 || cnt > DEYE_REQ_MAX) {
        rc = -10;
    } else {
        rc = rtu_read(port, slave, addr, cnt, result);
    }
    if (!s_deye_req.pending) return;             /* caller gave up meanwhile */
    if (!is_write && rc == 0) memcpy(s_deye_req.result, result, cnt * sizeof(uint16_t));
    s_deye_req.rc      = rc;
    s_deye_req.pending = false;
    s_deye_req.done    = true;
}

/* Somebody outside this task is waiting for the bus (a browser on /deye, a
 * Modbus-TCP client, the self-test). Checked between the live blocks so their
 * round-trip never queues behind the rest of our own poll round. */
static inline bool bus_has_waiter(int idx)
{
    return s_deye_req.pending || s_txn[idx].pending || s_selftest_req;
}

/* Backoff for the three optional live blocks (grid / output / PV). They exist on
 * an SG04LP3, but a model with a different register map would answer nothing and
 * each miss costs a full 400 ms read timeout -- three of those per 2 s round is
 * dead air on a bus that also carries the /deye probe and the TCP bridge. After
 * five misses in a row, ask that block only every 30 s.
 * One set of counters for all buses, like s_live itself: the design assumes one
 * Deye. */
#define LIVE_BLK_FAILS    5
#define LIVE_BLK_RETRY_MS 30000
static uint8_t s_blk_fail[3];
static int64_t s_blk_retry_us[3];

static bool live_blk_due(int b)
{
    return s_blk_fail[b] < LIVE_BLK_FAILS || esp_timer_get_time() >= s_blk_retry_us[b];
}

static void live_blk_done(int b, bool ok)
{
    if (ok) { s_blk_fail[b] = 0; return; }
    if (s_blk_fail[b] < 255) s_blk_fail[b]++;
    s_blk_retry_us[b] = esp_timer_get_time() + (int64_t)LIVE_BLK_RETRY_MS * 1000;
}

/* Read one optional live block into `r`, honouring the backoff and yielding to
 * anyone waiting for the bus. True when `r` holds fresh data. Skipping for a
 * waiter does NOT count as a miss -- nothing was asked. */
static bool live_read(int b, int idx, int port, uint8_t slave,
                      uint16_t addr, uint16_t cnt, uint16_t *r)
{
    if (bus_has_waiter(idx) || !live_blk_due(b)) return false;
    bool ok = (rtu_read(port, slave, addr, cnt, r) == 0);
    live_blk_done(b, ok);
    return ok;
}

/* Register scaling. Powers and currents can legitimately be negative, so they
 * are S16; voltages, frequency, SoC and the temperature never are -- reading
 * those as signed would only turn a garbled value into a plausible one. */
static inline float rs16(uint16_t v, float scale) { return (float)(int16_t)v * scale; }
static inline float ru16(uint16_t v, float scale) { return (float)v * scale; }

/* One poll round on a MASTER bus: the battery read that feeds the energy-flow
 * model, followed by the inverter's own live measurements for /api/deye/live.
 *
 * ~76 registers at 9600 baud are roughly 230 ms of the 2 s cadence. A block
 * that fails keeps its previous values (`blocks` says which ones are current),
 * and a failed battery read skips the rest of the round: three more 400 ms
 * timeouts would stall this task for over a second for nothing. */
static void poll_deye(int idx, int port, uint8_t slave)
{
    mb_deye_live_t l;
    portENTER_CRITICAL(&s_mux);
    l = s_live;                    /* carry over the blocks that fail below */
    portEXIT_CRITICAL(&s_mux);
    l.blocks = 0;

    uint16_t r[32];

    /* ---- battery, 586..592 (the model's input; also shown on the page) ---- */
    int rc = rtu_read(port, slave, 586, 7, r);
    l.online = (rc == 0);
    if (rc == 0) {
#define RG(a) r[(a) - 586]
        float soc = (float)RG(DEYE_SOC_REG);
        float w   = (float)(int16_t)RG(DEYE_PWR_REG);         /* +discharge */
        modbus_tcp_set_rtu_deye(w, soc, true);
        portENTER_CRITICAL(&s_mux);
        s_st.bus[idx].online = true; s_st.bus[idx].polls++;
        s_st.bus[idx].a = w; s_st.bus[idx].b = soc;
        portEXIT_CRITICAL(&s_mux);

        l.bat_temp = ru16(RG(586), 0.1f) - 100.0f;     /* raw*0.1-100 */
        l.bat_v    = ru16(RG(587), 0.01f);
        l.bat_soc  = soc;
        l.bat_p    = w;
        l.bat_i    = rs16(RG(591), 0.02f);
        l.blocks  |= MB_DEYE_BLK_BAT;
#undef RG
    } else {
        modbus_tcp_set_rtu_deye(0, 0, false);
        portENTER_CRITICAL(&s_mux);
        s_st.bus[idx].online = false; s_st.bus[idx].errs++;
        s_live.online = false; s_live.blocks = 0;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "bus%d Deye RTU read failed (%d)", idx, rc);
        return;
    }

    /* ---- grid side, 598..625 ---- */
    if (live_read(0, idx, port, slave, 598, 28, r)) {
#define RG(a) r[(a) - 598]
        for (int k = 0; k < 3; k++) {
            l.grid_v[k]       = ru16(RG(598 + k), 0.1f);
            l.grid_inner_p[k] = rs16(RG(604 + k), 1.0f);
            l.grid_ct_p[k]    = rs16(RG(616 + k), 1.0f);
            l.grid_p[k]       = rs16(RG(622 + k), 1.0f);
        }
        l.grid_inner_total = rs16(RG(607), 1.0f);
        l.grid_ct_total    = rs16(RG(619), 1.0f);
        l.grid_total       = rs16(RG(625), 1.0f);
        l.grid_freq        = ru16(RG(609), 0.01f);
        l.blocks |= MB_DEYE_BLK_GRID;
#undef RG
    }

    /* ---- inverter output + load, 627..655 ---- */
    if (live_read(1, idx, port, slave, 627, 29, r)) {
#define RG(a) r[(a) - 627]
        /* NOTE the negation on the AC output. The Deye reports its inverter
         * power the other way round from its own battery register: while the
         * battery charges it delivers -2803 W on the AC side (i.e. it DRAWS
         * 2803 W) and -2220 W at the battery. One rule has to win, and the
         * useful one is "+ = power flowing INTO the inverter at this port",
         * which the battery register already follows (+ = discharge = out of
         * the battery, into the inverter). So flip the AC side once, here, and
         * every consumer sees the same convention:
         *     +  the inverter DRAWS on the AC side  (charging the battery)
         *     -  the inverter DELIVERS on the AC side (feeding house/grid)
         * The load and UPS registers are left as the Deye reports them -- see
         * the note in modbus_rtu.h. */
        for (int k = 0; k < 3; k++) {
            l.inv_v[k]  = ru16(RG(627 + k), 0.1f);
            l.inv_i[k]  = rs16(RG(630 + k), 0.01f);
            l.inv_p[k]  = -rs16(RG(633 + k), 1.0f);
            l.ups_p[k]  = rs16(RG(640 + k), 1.0f);
            l.load_v[k] = ru16(RG(644 + k), 0.1f);
            l.load_i[k] = rs16(RG(647 + k), 0.01f);
            l.load_p[k] = rs16(RG(650 + k), 1.0f);
        }
        l.inv_total  = -rs16(RG(636), 1.0f);
        l.inv_freq   = ru16(RG(638), 0.01f);
        l.ups_total  = rs16(RG(643), 1.0f);
        l.load_total = rs16(RG(653), 1.0f);
        l.load_freq  = ru16(RG(655), 0.01f);
        l.blocks |= MB_DEYE_BLK_OUT;
#undef RG
    }

    /* ---- PV strings, 672..683 ---- */
    if (live_read(2, idx, port, slave, 672, 12, r)) {
#define RG(a) r[(a) - 672]
        l.pv_total = 0.0f;
        for (int k = 0; k < 4; k++) {
            l.pv_p[k]   = ru16(RG(672 + k),     1.0f);
            l.pv_v[k]   = ru16(RG(676 + 2 * k), 0.1f);
            l.pv_i[k]   = ru16(RG(677 + 2 * k), 0.1f);
            l.pv_total += l.pv_p[k];
        }
        l.blocks |= MB_DEYE_BLK_PV;
#undef RG
    }

    l.valid = true;
    portENTER_CRITICAL(&s_mux);
    s_live    = l;
    s_live_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_mux);
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

    if (rtu_read_exact(port, resp, 2, deadline) < 0) { rtu_drain(port); return -12; }  /* timeout */
    got = 2;

    int body;                       /* bytes still outstanding after the header */
    if (resp[1] & 0x80) {
        body = 3;                                   /* exception code + CRC     */
    } else switch (resp[1]) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x17:
        if (rtu_read_exact(port, resp + 2, 1, deadline) < 0) { rtu_drain(port); return -12; }
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
    if (rtu_read_exact(port, resp + got, body, deadline) < 0) { rtu_drain(port); return -12; }
    got += body;

    if ((resp[got - 2] | (resp[got - 1] << 8)) != crc16(resp, got - 2)) return -15;
    if (resp[0] != req[0]) return -16;              /* answer from another id   */
    if ((resp[1] & 0x7F) != req[1]) return -17;     /* answer to another request */

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
        poll_deye(idx, port, c->slave_id);
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

/* The setpoint shifts what the inverter is told the grid does, so a runaway
 * value here is a runaway inverter. The web form already refuses anything
 * beyond +-30 kW; the module itself accepted any int -- also straight from
 * NVS at boot, where a corrupted record would have been applied silently. */
#define MB_GRID_SP_MAX_W 30000
static int clamp_grid_sp(int w)
{
    if (w >  MB_GRID_SP_MAX_W) return  MB_GRID_SP_MAX_W;
    if (w < -MB_GRID_SP_MAX_W) return -MB_GRID_SP_MAX_W;
    return w;
}

void modbus_rtu_set_grid_setpoint(int w)
{
    w = clamp_grid_sp(w);
    s_grid_sp = w;
    nvs_store_set_grid_sp(w);
}

const char *modbus_rtu_phase_mode_name(uint8_t mode)
{
    switch (mode) {
    case MB_PH_OFFSET: return "offset";
    case MB_PH_ABS:    return "absolut";
    case MB_PH_SCALE:  return "skalieren";
    default:           return "aus";
    }
}

void modbus_rtu_get_manip(mb_manip_cfg_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_manip;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t modbus_rtu_manip_left_s(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t left = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_manip.enabled) {
        uint32_t age = (now - s_manip_ms) / 1000u;
        left = (age >= MB_MANIP_MAX_S) ? 0 : (uint32_t)MB_MANIP_MAX_S - age;
    }
    portEXIT_CRITICAL(&s_mux);
    return left;
}

esp_err_t modbus_rtu_set_manip(const mb_manip_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    mb_manip_cfg_t m = *cfg;
    clamp_manip(&m);
    esp_err_t e = nvs_store_set_mb_manip(&m, sizeof(m));
    portENTER_CRITICAL(&s_mux);
    bool was_on = s_manip.enabled;
    s_manip = m;                 /* apply immediately, even if NVS refused */
    if (m.enabled && !was_on) s_manip_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "phase manipulation %s: L1=%s/%.0f L2=%s/%.0f L3=%s/%.0f",
             m.enabled ? "ON" : "off",
             modbus_rtu_phase_mode_name(m.ph[0].mode), m.ph[0].value,
             modbus_rtu_phase_mode_name(m.ph[1].mode), m.ph[1].value,
             modbus_rtu_phase_mode_name(m.ph[2].mode), m.ph[2].value);
    return e;
}

/* Snapshot of the inverter's own live measurements. `age_ms` counts from the
 * last successful poll round, so the page can grey the values out when the
 * master bus goes quiet. */
void modbus_rtu_get_deye_live(mb_deye_live_t *out)
{
    if (!out) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_mux);
    *out = s_live;
    out->age_ms = s_live_ms ? (uint32_t)(now - s_live_ms) : 0;
    portEXIT_CRITICAL(&s_mux);
}

void modbus_rtu_get_served(mb_served_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* Recompute rather than report the last answered request: the page must show
     * the chain even on a setup where no bus is in slave mode at all. */
    float grid_w = 0.0f;
    bool  fresh  = modbus_tcp_grid_w_fresh(&grid_w, MB_GRID_MAX_AGE_MS);
    bool  per_phase = false;
    float real[3], p[3], total = 0.0f;
    compute_served(fresh, grid_w, &per_phase, real, p, &total);

    out->fresh        = fresh;
    out->per_phase    = per_phase;
    out->setpoint     = s_grid_sp;
    out->real_total   = fresh ? (per_phase ? real[0] + real[1] + real[2] : grid_w) : 0.0f;
    out->served_total = total;
    for (int k = 0; k < 3; k++) {
        out->real_p[k]   = real[k];
        out->served_p[k] = p[k];
        out->served_v[k] = SDM_NOMINAL_V;
        out->served_i[k] = fabsf(p[k]) / SDM_NOMINAL_V;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_mux);
    out->requests = s_requests;
    out->age_ms   = s_request_ms ? (uint32_t)(now - s_request_ms) : 0;
    bool slave = false;
    for (int i = 0; i < MB_RTU_BUSES; i++)
        if (s_cfg.bus[i].enabled && s_cfg.bus[i].role == MB_RTU_SLAVE) slave = true;
    out->slave_running = slave;
    out->quiet   = s_slave_quiet;
    out->hold_s  = (s_cfg.slave_hold_s == MB_SLAVE_HOLD_FOREVER)
                       ? 0 : (s_cfg.slave_hold_s ? s_cfg.slave_hold_s
                                                 : MB_SLAVE_HOLD_DEFAULT_S);
    out->stale_s = s_stale_ms / 1000u;
    portEXIT_CRITICAL(&s_mux);
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
    bool finished = false;
    for (int i = 0; i < 60; i++) {             /* up to ~3 s */
        if (s_deye_req.done) {
            rc = s_deye_req.rc;
            if (rc == 0 && !is_write && out)
                for (int k = 0; k < count; k++) out[k] = s_deye_req.result[k];
            finished = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!finished) {
        /* The bus task may be mid-exchange on THIS request. Releasing the mutex
         * now let its completion land on the next caller, who then saw `done`
         * immediately and took the previous request's rc/result as its own
         * (a NORMAL-mode write sequence "succeeded" with one register never
         * written). Give it the same grace modbus_rtu_txn() gives. */
        for (int i = 0; i < 60 && !s_deye_req.done; i++)
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
    s_grid_sp = clamp_grid_sp(nvs_store_get_grid_sp());
    if (s_grid_sp != 0)
        ESP_LOGW(TAG, "grid setpoint from NVS: %d W (meter reports real - setpoint)", s_grid_sp);
    load_manip();
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

    mb_rtu_cfg_t cur;
    portENTER_CRITICAL(&s_mux);
    cur = s_cfg;
    portEXIT_CRITICAL(&s_mux);

    /* Every widget in the "Mod RTU" tab saves the whole struct on each change,
     * so re-opening a dropdown and picking the same entry used to cost an NVS
     * write and a blanked Deye tile. Nothing changed -> do nothing. (Differing
     * struct padding can only make this MISS a no-op, never invent one.) */
    if (memcmp(&cur, &c, sizeof(c)) == 0) return ESP_OK;

    /* Only a changed bus invalidates what the master has read; the bridge
     * fields (port, client mask) must not blank the Deye reading. */
    bool bus_changed = memcmp(cur.bus, c.bus, sizeof(cur.bus)) != 0;

    esp_err_t e = nvs_store_set_mb_rtu(&c, sizeof(c));
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_cfg = c;
        if (bus_changed) { s_live.online = false; s_live.blocks = 0; }
        portEXIT_CRITICAL(&s_mux);
        /* clear stale Deye value; an enabled master re-populates within 2 s */
        if (bus_changed) modbus_tcp_set_rtu_deye(0, 0, false);
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

#include "harness.h"
#include "fakes.h"

/* The unit under test, included as source: the interesting functions
 * (compute_served, crc16, sdm630_response) are static, and they are static for
 * a good reason -- nothing outside this file may call them. Testing them
 * without first refactoring the live control path means compiling the
 * translation unit right here. */
#include "modbus_rtu.c"

/* ------------------------------- helpers ------------------------------- */

static void reset_all(void)
{
    fake_nvs_reset();
    fake_uart_reset();
    fake_grid_fresh        = false;
    fake_grid_w            = 0;
    fake_grid_phases_fresh = false;
    memset(fake_grid_phases, 0, sizeof(fake_grid_phases));
    memset(&s_manip, 0, sizeof(s_manip));
    s_grid_sp = 0;
}

static void manip_set(int phase, uint8_t mode, float value)
{
    s_manip.enabled       = 1;
    s_manip.ph[phase].mode  = mode;
    s_manip.ph[phase].value = value;
}

/* ------------- the safety property: stale means zero --------------------
 * This is the one that cost 15 kW of export once. Nothing -- no setpoint, no
 * manipulation, no leftover reading -- may put a non-zero number in front of
 * the inverter while the grid reading is stale. */

static void test_stale_serves_zero(void)
{
    reset_all();
    float p[3], real[3], total = -1;
    bool per_phase = true;

    /* A big, fresh-looking value sitting in the fake, but fresh = false. */
    fake_grid_w            = 9000;
    fake_grid_phases_fresh = true;
    fake_grid_phases[0] = 3000; fake_grid_phases[1] = 3000; fake_grid_phases[2] = 3000;
    modbus_rtu_set_grid_setpoint(-5000);
    manip_set(0, MB_PH_ABS, 12000);        /* and manipulation turned up hard */

    compute_served(false, 9000, &per_phase, real, p, &total);

    CHECK_F(p[0], 0, 0);
    CHECK_F(p[1], 0, 0);
    CHECK_F(p[2], 0, 0);
    CHECK_F(total, 0, 0);
    CHECK(!per_phase);                     /* no per-phase claim when stale */
    CHECK_F(real[0], 0, 0);                /* and no real value leaks out    */
    CHECK_F(real[1], 0, 0);
    CHECK_F(real[2], 0, 0);
}

/* ---------------- the zero-export trick, split evenly ------------------ */

static void test_fresh_no_phase_data_splits_evenly(void)
{
    reset_all();
    fake_grid_phases_fresh = false;        /* meter without per-phase data */
    float p[3], real[3], total = 0;
    bool per_phase = true;

    compute_served(true, 900, &per_phase, real, p, &total);

    CHECK(!per_phase);
    CHECK_F(real[0], 300, 0.01);
    CHECK_F(p[0], 300, 0.01);
    CHECK_F(p[1], 300, 0.01);
    CHECK_F(p[2], 300, 0.01);
    CHECK_F(total, 900, 0.01);
}

static void test_setpoint_shifts_what_the_deye_sees(void)
{
    reset_all();
    /* "Please export 350 W": the Deye is told 350 W MORE than is really
     * flowing, so it keeps pushing until the real point sits at -350 W. */
    modbus_rtu_set_grid_setpoint(-350);
    float p[3], total = 0;

    compute_served(true, 0, NULL, NULL, p, &total);

    CHECK_F(p[0], 350.0 / 3.0, 0.01);
    CHECK_F(total, 350, 0.01);

    /* And once the real flow HAS reached -350 W, the Deye must see zero --
     * otherwise it would keep turning up. That is the whole trick. */
    compute_served(true, -350, NULL, NULL, p, &total);
    CHECK_F(total, 0, 0.01);
}

static void test_per_phase_data_is_used_as_is(void)
{
    reset_all();
    fake_grid_phases_fresh = true;
    fake_grid_phases[0] = 100; fake_grid_phases[1] = 200; fake_grid_phases[2] = -300;
    float p[3], real[3], total = 0;
    bool per_phase = false;

    compute_served(true, 0, &per_phase, real, p, &total);

    CHECK(per_phase);
    CHECK_F(p[0], 100, 0.01);
    CHECK_F(p[1], 200, 0.01);
    CHECK_F(p[2], -300, 0.01);
    CHECK_F(total, 0, 0.01);               /* imbalance, but balanced in sum */
    CHECK_F(real[2], -300, 0.01);
}

/* ------------------------ per-phase manipulation ----------------------- */

static void test_manipulation_off_changes_nothing(void)
{
    reset_all();
    s_manip.enabled = 0;                   /* master switch off ... */
    s_manip.ph[0].mode  = MB_PH_ABS;       /* ... but a value left behind */
    s_manip.ph[0].value = 5000;
    float p[3], total = 0;

    compute_served(true, 900, NULL, NULL, p, &total);

    CHECK_F(p[0], 300, 0.01);              /* untouched */
    CHECK_F(total, 900, 0.01);
}

static void test_manipulation_modes(void)
{
    float p[3], total = 0;

    reset_all();
    manip_set(0, MB_PH_OFFSET, 500);
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK_F(p[0], 800, 0.01);              /* 300 + 500 */
    CHECK_F(p[1], 300, 0.01);

    reset_all();
    manip_set(1, MB_PH_ABS, -1234);
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK_F(p[1], -1234, 0.01);
    CHECK_F(p[0], 300, 0.01);

    reset_all();
    manip_set(2, MB_PH_SCALE, 50);         /* percent */
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK_F(p[2], 150, 0.01);
}

static void test_manipulation_rejects_garbage(void)
{
    float p[3], total = 0;

    /* NaN must never reach the inverter. */
    reset_all();
    manip_set(0, MB_PH_ABS, NAN);
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK(isfinite(p[0]));
    CHECK_F(p[0], 0, 0.01);

    /* Neither must an absurd number: everything served is clamped. */
    reset_all();
    manip_set(0, MB_PH_ABS, 5e6);
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK_F(p[0], MB_SERVED_CLAMP_W, 0.01);

    reset_all();
    manip_set(0, MB_PH_ABS, -5e6);
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK_F(p[0], -MB_SERVED_CLAMP_W, 0.01);

    /* An unknown mode (a config from a newer firmware) passes through. */
    reset_all();
    manip_set(0, 99, 4711);
    compute_served(true, 900, NULL, NULL, p, &total);
    CHECK_F(p[0], 300, 0.01);
}

/* ------------------------------ setpoint -------------------------------- */

static void test_setpoint_is_bounded(void)
{
    reset_all();
    modbus_rtu_set_grid_setpoint(999999);
    CHECK_I(modbus_rtu_get_grid_setpoint(), MB_GRID_SP_MAX_W);
    modbus_rtu_set_grid_setpoint(-999999);
    CHECK_I(modbus_rtu_get_grid_setpoint(), -MB_GRID_SP_MAX_W);
    modbus_rtu_set_grid_setpoint(-350);
    CHECK_I(modbus_rtu_get_grid_setpoint(), -350);
}

/* --------------------------- Modbus framing ---------------------------- */

/* Cross-check against an independent implementation: the expected values were
 * produced by a Python CRC-16/MODBUS (see test/README.md), not by this code. */
static void test_crc16_matches_reference(void)
{
    const uint8_t read_req[6]  = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    const uint8_t sdm_req[6]   = { 0x01, 0x04, 0x00, 0x0C, 0x00, 0x02 };
    const uint8_t empty[1]     = { 0x00 };

    CHECK_I(crc16(read_req, 6), 0xCDC5);
    CHECK_I(crc16(sdm_req, 6),  0xC8B1);
    CHECK_I(crc16(empty, 1),    0x40BF);
}

static void test_sdm630_response_shape(void)
{
    reset_all();
    /* FC04, 2 registers from 0x000C (L1 power in the SDM630 map). */
    const uint8_t req[8] = { 0x01, 0x04, 0x00, 0x0C, 0x00, 0x02, 0x00, 0x00 };
    uint8_t out[260];
    float p[3] = { 1234.5f, 0, 0 };

    int n = sdm630_response(req, out, p, 1234.5f);

    CHECK_I(n, 3 + 2 * 2 + 2);             /* header + 2 regs + CRC */
    CHECK_I(out[0], 0x01);                 /* echoes slave id ... */
    CHECK_I(out[1], 0x04);                 /* ... and function code */
    CHECK_I(out[2], 4);                    /* byte count */

    /* The two registers carry one IEEE-754 float, high word first. */
    uint32_t u = ((uint32_t)out[3] << 24) | ((uint32_t)out[4] << 16) |
                 ((uint32_t)out[5] << 8)  |  (uint32_t)out[6];
    float f;
    memcpy(&f, &u, sizeof(f));
    CHECK_F(f, 1234.5, 0.01);

    /* And the CRC over the whole frame checks out. */
    uint16_t c = crc16(out, n - 2);
    CHECK_I(out[n - 2], c & 0xFF);
    CHECK_I(out[n - 1], c >> 8);

    /* A request for zero or too many registers is refused, not answered. */
    const uint8_t bad0[8] = { 0x01, 0x04, 0x00, 0x0C, 0x00, 0x00, 0, 0 };
    const uint8_t bad200[8] = { 0x01, 0x04, 0x00, 0x0C, 0x00, 200, 0, 0 };
    CHECK_I(sdm630_response(bad0, out, p, 0), 0);
    CHECK_I(sdm630_response(bad200, out, p, 0), 0);
}

/* ---------------------- config: defaults and the gate ------------------ */

static void test_clamp_cfg_fills_defaults(void)
{
    mb_rtu_cfg_t c;
    memset(&c, 0, sizeof(c));              /* a blob from an older firmware */

    clamp_cfg(&c);

    CHECK_I(c.bus[0].baud, 9600);
    CHECK_I(c.bus[1].baud, 9600);
    CHECK_I(c.bus[0].slave_id, 1);
    CHECK_I(c.gw_port, MB_GW_DEFAULT_PORT);
    CHECK_I(c.gw_timeout_ms, MB_GW_DEFAULT_TIMEOUT_MS);
    CHECK_I(c.gw_max_clients, MB_GW_DEFAULT_CLIENTS);
    /* 0 stays 0 here: the meaning "use the default" is resolved where it is
     * used (do_slave), so a changed default still reaches an old device. */
    CHECK_I(c.slave_hold_s, 0);

    /* Bridge on without a bus picked -> serve every bus, rather than
     * answering 0x0A to everything. */
    memset(&c, 0, sizeof(c));
    c.gw_enabled = 1;
    clamp_cfg(&c);
    CHECK_I(c.gw_bus_mask, (1u << MB_RTU_BUSES) - 1u);

    /* An out-of-range role falls back to master, never to "slave by
     * accident" -- a bus that emulates a meter without being told to would
     * talk to the inverter. */
    memset(&c, 0, sizeof(c));
    c.bus[0].role = 42;
    clamp_cfg(&c);
    CHECK_I(c.bus[0].role, MB_RTU_MASTER);
}

static void test_set_cfg_is_a_noop_when_nothing_changed(void)
{
    reset_all();
    mb_rtu_cfg_t c;
    memset(&c, 0, sizeof(c));
    clamp_cfg(&c);
    CHECK_I(modbus_rtu_set_cfg(&c), ESP_OK);      /* first write goes through */

    /* The Deye reading was invalidated by that first (real) change ... */
    fake_rtu_deye_valid = true;
    /* ... and writing the same thing again must not touch anything: no NVS
     * write, and above all no blanked Deye value. Every widget in the Mod-RTU
     * tab saves the whole struct on each change, so this is the common case. */
    fake_nvs_fail = true;                          /* would fail IF it wrote */
    CHECK_I(modbus_rtu_set_cfg(&c), ESP_OK);
    CHECK(fake_rtu_deye_valid);                    /* untouched */
}

static void test_bridge_change_keeps_the_deye_reading(void)
{
    reset_all();
    mb_rtu_cfg_t c;
    memset(&c, 0, sizeof(c));
    clamp_cfg(&c);
    modbus_rtu_set_cfg(&c);

    fake_rtu_deye_valid = true;
    c.gw_port = 1502;                              /* bridge only */
    CHECK_I(modbus_rtu_set_cfg(&c), ESP_OK);
    CHECK(fake_rtu_deye_valid);                    /* still there */

    fake_rtu_deye_valid = true;
    c.bus[0].baud = 19200;                         /* a bus, though */
    CHECK_I(modbus_rtu_set_cfg(&c), ESP_OK);
    CHECK(!fake_rtu_deye_valid);                   /* dropped, as it must be */
}

int main(void)
{
    RUN(test_stale_serves_zero);
    RUN(test_fresh_no_phase_data_splits_evenly);
    RUN(test_setpoint_shifts_what_the_deye_sees);
    RUN(test_per_phase_data_is_used_as_is);
    RUN(test_manipulation_off_changes_nothing);
    RUN(test_manipulation_modes);
    RUN(test_manipulation_rejects_garbage);
    RUN(test_setpoint_is_bounded);
    RUN(test_crc16_matches_reference);
    RUN(test_sdm630_response_shape);
    RUN(test_clamp_cfg_fills_defaults);
    RUN(test_set_cfg_is_a_noop_when_nothing_changed);
    RUN(test_bridge_change_keeps_the_deye_reading);
    return t_report("modbus_rtu");
}

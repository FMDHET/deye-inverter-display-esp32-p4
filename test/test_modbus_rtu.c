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

/* ----------------- the whole slave path, request to answer -------------
 * do_slave() is where the emulation actually talks to the inverter, and where
 * the bridge-then-silence rule lives. With a queue on the fake UART the whole
 * path is testable -- including the part that needed a real meter outage on a
 * live inverter to observe (Nachtrag 4). */

/* An SDM630 request as the Deye sends it: FC04, 2 registers from `addr`. */
static void push_request(uint8_t slave, uint8_t fc, uint16_t addr, uint16_t cnt)
{
    uint8_t req[8] = { slave, fc, (uint8_t)(addr >> 8), (uint8_t)addr,
                       (uint8_t)(cnt >> 8), (uint8_t)cnt, 0, 0 };
    uint16_t c = crc16(req, 6);
    req[6] = (uint8_t)(c & 0xFF);
    req[7] = (uint8_t)(c >> 8);
    fake_uart_rx_push(req, sizeof(req));
}

static mb_rtu_bus_cfg_t slave_bus(void)
{
    mb_rtu_bus_cfg_t c = { .enabled = 1, .role = MB_RTU_SLAVE, .slave_id = 1, .baud = 9600 };
    return c;
}

/* Drives do_slave() from a known state: fresh reading first, so the internal
 * fresh/stale edge detector is where the test wants it. */
static void slave_warmup(void)
{
    mb_rtu_bus_cfg_t c = slave_bus();
    fake_grid_fresh = true;
    fake_grid_w = 0;
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    fake_uart_reset();
}

static float answered_l1(void)
{
    /* out[3..6] = L1 power as an IEEE-754 float, high word first. */
    uint32_t u = ((uint32_t)fake_uart_tx[3] << 24) | ((uint32_t)fake_uart_tx[4] << 16) |
                 ((uint32_t)fake_uart_tx[5] << 8)  |  (uint32_t)fake_uart_tx[6];
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static void test_slave_answers_a_valid_request(void)
{
    reset_all();
    fake_time_set_ms(1000);
    slave_warmup();
    mb_rtu_bus_cfg_t c = slave_bus();

    fake_grid_fresh = true;
    fake_grid_w     = 900;
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);

    CHECK_I(fake_uart_writes, 1);
    CHECK_I(fake_uart_tx[0], 1);           /* our slave id */
    CHECK_I(fake_uart_tx[1], 4);
    CHECK_F(answered_l1(), 300, 0.01);     /* 900 W split over three phases */
}

static void test_slave_ignores_what_is_not_for_it(void)
{
    mb_rtu_bus_cfg_t c = slave_bus();

    reset_all(); fake_time_set_ms(1000); slave_warmup();
    push_request(2, 4, 0x0C, 2);           /* someone else's slave id */
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 0);

    reset_all(); fake_time_set_ms(1000); slave_warmup();
    push_request(1, 6, 0x0C, 2);           /* a write function code */
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 0);

    /* A corrupted frame: right length, wrong checksum. */
    reset_all(); fake_time_set_ms(1000); slave_warmup();
    uint8_t bad[8] = { 1, 4, 0, 0x0C, 0, 2, 0xAA, 0xBB };
    fake_uart_rx_push(bad, sizeof(bad));
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 0);
}

static void test_stale_bridges_with_zero_then_goes_silent(void)
{
    reset_all();
    fake_time_set_ms(100000);
    slave_warmup();                        /* starts fresh */
    mb_rtu_bus_cfg_t c = slave_bus();

    /* The reading goes stale. For the next minute the meter stays alive and
     * answers a balanced zero -- the Deye holds instead of chasing. */
    fake_grid_fresh = false;
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 1);
    CHECK_F(answered_l1(), 0, 0.01);
    CHECK(!s_slave_quiet);

    fake_time_advance_ms(59 * 1000);       /* still inside the bridge */
    fake_uart_reset();
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 1);
    CHECK_F(answered_l1(), 0, 0.01);

    /* Past the bridge: silence, so the inverter falls back to its own CT
     * instead of holding a number nobody is updating. */
    fake_time_advance_ms(3 * 1000);
    fake_uart_reset();
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 0);
    CHECK(s_slave_quiet);

    /* And when the meter comes back, so does the emulation. */
    fake_grid_fresh = true;
    fake_grid_w     = 600;
    fake_uart_reset();
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 1);
    CHECK_F(answered_l1(), 200, 0.01);
    CHECK(!s_slave_quiet);
}

static void test_hold_forever_keeps_the_old_behaviour(void)
{
    reset_all();
    fake_time_set_ms(100000);
    slave_warmup();
    mb_rtu_bus_cfg_t c = slave_bus();
    s_cfg.slave_hold_s = MB_SLAVE_HOLD_FOREVER;

    fake_grid_fresh = false;
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);                    /* enters the stale state */

    fake_time_advance_ms(60 * 60 * 1000);  /* an hour later */
    fake_uart_reset();
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_I(fake_uart_writes, 1);          /* still answering */
    CHECK_F(answered_l1(), 0, 0.01);
    CHECK(!s_slave_quiet);
    s_cfg.slave_hold_s = 0;
}

static void test_manipulation_expires_on_its_own(void)
{
    reset_all();
    fake_time_set_ms(500000);
    slave_warmup();
    mb_rtu_bus_cfg_t c = slave_bus();

    fake_grid_fresh = true;
    fake_grid_w     = 900;
    manip_set(0, MB_PH_ABS, 7000);
    s_manip_ms = (uint32_t)(fake_now_us / 1000);

    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_F(answered_l1(), 7000, 0.01);            /* manipulated, as asked */
    CHECK(s_manip.enabled);
    CHECK(modbus_rtu_manip_left_s() > 0);

    /* Just before the limit it is still in force ... */
    fake_time_advance_ms((MB_MANIP_MAX_S - 60) * 1000);
    fake_uart_reset();
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK_F(answered_l1(), 7000, 0.01);
    CHECK(s_manip.enabled);

    /* ... and past it, the inverter gets the honest number again. */
    fake_time_advance_ms(2 * 60 * 1000);
    fake_uart_reset();
    push_request(1, 4, 0x0C, 2);
    do_slave(0, 0, &c);
    CHECK(!s_manip.enabled);
    CHECK_F(answered_l1(), 300, 0.01);
    CHECK_I(modbus_rtu_manip_left_s(), 0);
}

static void test_manipulation_is_off_after_a_restart(void)
{
    /* Manipulation lives only in this device. Resuming it after an unattended
     * restart would mean quietly feeding the inverter wrong numbers with
     * nobody watching -- so the boot path switches it off, in RAM and in
     * flash, and keeps the values for the next deliberate switch-on. */
    reset_all();
    fake_time_set_ms(1000);

    mb_manip_cfg_t stored;
    memset(&stored, 0, sizeof(stored));
    stored.enabled       = 1;
    stored.ph[1].mode    = MB_PH_ABS;
    stored.ph[1].value   = 4200;
    CHECK_I(nvs_store_set_mb_manip(&stored, sizeof(stored)), ESP_OK);

    load_manip();

    CHECK(!s_manip.enabled);                    /* off in RAM ... */
    CHECK_F(s_manip.ph[1].value, 4200, 0.01);   /* ... values kept */
    CHECK_I(s_manip.ph[1].mode, MB_PH_ABS);

    mb_manip_cfg_t back;
    memset(&back, 0, sizeof(back));
    CHECK_I(nvs_store_get_mb_manip(&back, sizeof(back)), ESP_OK);
    CHECK(!back.enabled);                       /* ... and off in flash */
    CHECK_F(back.ph[1].value, 4200, 0.01);

    /* An untouched config must not be rewritten on every boot. */
    reset_all();
    memset(&stored, 0, sizeof(stored));
    stored.ph[0].mode = MB_PH_OFFSET;
    stored.ph[0].value = 100;
    nvs_store_set_mb_manip(&stored, sizeof(stored));
    fake_nvs_fail = true;                       /* any write would fail now */
    load_manip();
    CHECK(!s_manip.enabled);
    fake_nvs_fail = false;
}

/* ---------------------- self-test bus selection ------------------------
 * The test asks over one bus and is answered over the other. It used to be
 * wired to "bus 1 asks, bus 0 answers" regardless of configuration -- which
 * on this installation (bus 0 master, bus 1 slave) meant transmitting onto
 * the bus the Deye is actively polling, asking for the Deye's own id, and
 * reporting FAIL every time. The pair now comes from the roles. */

static mb_rtu_cfg_t pair_cfg(uint8_t master_idx)
{
    mb_rtu_cfg_t c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        c.bus[i].enabled  = 1;
        c.bus[i].baud     = 9600;
        c.bus[i].slave_id = (uint8_t)(i + 1);
        c.bus[i].role     = (i == master_idx) ? MB_RTU_MASTER : MB_RTU_SLAVE;
    }
    return c;
}

static void test_selftest_accepts_either_bus_as_master(void)
{
    /* Both layouts are legitimate; neither may be privileged. */
    mb_rtu_cfg_t a = pair_cfg(0);
    CHECK(selftest_why_not(&a) == NULL);
    mb_rtu_cfg_t b = pair_cfg(1);
    CHECK(selftest_why_not(&b) == NULL);
}

static void test_selftest_refuses_without_a_partner(void)
{
    /* Two masters: the request would go out and nothing could answer it. */
    mb_rtu_cfg_t c = pair_cfg(0);
    c.bus[1].role = MB_RTU_MASTER;
    CHECK(selftest_why_not(&c) != NULL);

    /* Two slaves: nobody to ask with. */
    c = pair_cfg(0);
    c.bus[0].role = MB_RTU_SLAVE;
    CHECK(selftest_why_not(&c) != NULL);
}

static void test_selftest_refuses_when_a_bus_is_switched_off(void)
{
    /* A disabled bus cannot answer -- and the old code did not even look at
     * `enabled`: the check sat in FRONT of it, so the test transmitted on a
     * bus the operator had switched off. */
    mb_rtu_cfg_t c = pair_cfg(0);
    c.bus[1].enabled = 0;
    CHECK(selftest_why_not(&c) != NULL);

    c = pair_cfg(0);
    c.bus[0].enabled = 0;
    CHECK(selftest_why_not(&c) != NULL);

    c = pair_cfg(0);
    c.bus[0].enabled = c.bus[1].enabled = 0;
    CHECK(selftest_why_not(&c) != NULL);
}

static void test_selftest_says_which_half_is_missing(void)
{
    /* The reason reaches the operator on the display, so it has to name the
     * missing half rather than just "failed". */
    mb_rtu_cfg_t c = pair_cfg(0);
    c.bus[1].role = MB_RTU_MASTER;          /* no slave */
    CHECK(strstr(selftest_why_not(&c), "Slave") != NULL);

    c = pair_cfg(0);
    c.bus[0].role = MB_RTU_SLAVE;           /* no master */
    CHECK(strstr(selftest_why_not(&c), "Master") != NULL);
}

/* ------------------------------ HOLD ----------------------------------- */

static void hold_live(float inv_total)
{
    s_live.valid = true;
    s_live.blocks = MB_DEYE_BLK_ALL;
    s_live.inv_total = inv_total;
    s_live_ms = (uint32_t)(fake_now_us / 1000);
}

static void test_hold_ramps_a_discharging_deye_to_zero(void)
{
    reset_all();
    fake_grid_fresh = true;
    fake_grid_w = 400;                       /* still importing 400 W     */
    hold_live(-1200);                        /* Deye delivers 1200 W      */
    modbus_rtu_set_hold(600);
    float total = 0;
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, -1200, 0.5);              /* "you export your output"  */

    hold_live(0);                            /* it has ramped down        */
    fake_grid_w = 1600;                      /* the grid now covers it    */
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, 0, 0.5);                  /* equilibrium at 0 W        */
    modbus_rtu_set_hold(0);
}

static void test_hold_still_takes_a_surplus(void)
{
    reset_all();
    fake_grid_fresh = true;
    fake_grid_w = -2000;                     /* Fronius exports 2 kW      */
    hold_live(800);                          /* Deye charges 800 W        */
    modbus_rtu_set_hold(600);
    float total = 0;
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, -2000, 0.5);              /* normal: keep absorbing    */
    modbus_rtu_set_hold(0);
}

/* The operator's lock is a second source of the same hold: it holds alone,
 * and the energy manager lifting ITS hold does not lift the lock. */
static void test_lock_holds_and_survives_the_managers_release(void)
{
    reset_all();
    fake_grid_fresh = true;
    fake_grid_w = 400;
    hold_live(-1200);
    modbus_rtu_set_lock(120);
    CHECK(modbus_rtu_lock_active());
    CHECK(!modbus_rtu_hold_active());
    float total = 0;
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, -1200, 0.5);
    modbus_rtu_set_hold(600);
    modbus_rtu_set_hold(0);                  /* the manager lets go ...   */
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, -1200, 0.5);              /* ... the lock still holds  */
    fake_now_us += 121LL * 1000000;          /* not renewed: it expires   */
    hold_live(-1200);
    CHECK(!modbus_rtu_lock_active());
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, 400, 0.5);                /* normal again              */
    modbus_rtu_set_lock(0);
}

static void test_hold_never_touches_a_stale_reading(void)
{
    reset_all();
    hold_live(-1200);
    modbus_rtu_set_hold(600);
    float total = 123;
    compute_served(false, 0, NULL, NULL, NULL, &total);
    CHECK_F(total, 0, 0.001);                /* stale is still 0          */
    modbus_rtu_set_hold(0);
}

static void test_hold_expires_and_needs_fresh_output(void)
{
    reset_all();
    fake_grid_fresh = true;
    fake_grid_w = 400;
    hold_live(-1200);
    modbus_rtu_set_hold(60);
    CHECK(modbus_rtu_hold_active());
    fake_time_advance_ms(61000);
    CHECK(!modbus_rtu_hold_active());
    float total = 0;
    hold_live(-1200);
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, 400, 0.5);                /* expired: normal again     */

    modbus_rtu_set_hold(600);
    fake_time_advance_ms(7000);              /* output reading 7 s old    */
    compute_served(true, fake_grid_w, NULL, NULL, NULL, &total);
    CHECK_F(total, 400, 0.5);                /* no guessing: normal       */
    modbus_rtu_set_hold(0);
}

/* --------------------- efficiency by power (the curve) ------------------ */

static void reset_curve(void)
{
    fake_nvs_reset_eff();
    memset(&s_curve, 0, sizeof(s_curve));
    s_curve.ver = MB_EFF_CURVE_VER; s_curve.nbins = MB_EFF_BINS;
    s_curve_dirty = false;
}

static void test_bins_follow_the_edges(void)
{
    CHECK_I(mb_eff_bin_of(0), 0);
    CHECK_I(mb_eff_bin_of(149.9f), 0);
    CHECK_I(mb_eff_bin_of(150), 1);
    CHECK_I(mb_eff_bin_of(999), 4);
    CHECK_I(mb_eff_bin_of(1000), 5);
    CHECK_I(mb_eff_bin_of(1249), 5);
    CHECK_I(mb_eff_bin_of(1250), 6);            /* 250 W steps from 1 to 5 kW */
    CHECK_I(mb_eff_bin_of(4999), 20);
    CHECK_I(mb_eff_bin_of(5000), 21);
    CHECK_I(mb_eff_bin_of(19999), 27);
    CHECK_I(mb_eff_bin_of(20000), 28);
    CHECK_I(mb_eff_bin_of(45000), 28);          /* the last bin is open */
    CHECK_I(MB_EFF_BIN_LO[MB_EFF_BINS - 1], 20000);
    for (int i = 1; i < MB_EFF_BINS; i++) CHECK(MB_EFF_BIN_LO[i] > MB_EFF_BIN_LO[i - 1]);
}

static void test_a_round_adds_energy_and_time(void)
{
    reset_curve();
    eff_bin_add(&s_curve.dis[5], 1000.0f, 940.0f, 2000000);    /* 2 s */
    eff_bin_add(&s_curve.dis[5], 1000.0f, -30.0f, 2000000);    /* AC drawn: out 0 */
    CHECK_F(s_curve.dis[5].in_wh, 2000.0 * 2 / 3600, 1e-4);
    CHECK_F(s_curve.dis[5].out_wh, 940.0 * 2 / 3600, 1e-4);
    CHECK_I(s_curve.dis[5].s, 4);
    CHECK(s_curve_dirty);
}

static void test_the_curve_survives_a_restart(void)
{
    reset_curve();
    eff_bin_add(&s_curve.chg[17], 4000.0f, 3800.0f, 2000000);
    eff_bin_add(&s_curve.idle, 60.0f, 0.0f, 2000000);
    eff_curve_save(true);
    CHECK(!s_curve_dirty);

    mb_eff_curve_t before = s_curve;
    memset(&s_curve, 0, sizeof(s_curve));
    eff_curve_load();
    CHECK(memcmp(&s_curve, &before, sizeof(before)) == 0);
}

static void test_a_failed_save_is_retried(void)
{
    reset_curve();
    eff_bin_add(&s_curve.dis[3], 600.0f, 540.0f, 2000000);
    fake_nvs_fail = true;
    eff_curve_save(true);
    CHECK(s_curve_dirty);                       /* still owed */
    fake_nvs_fail = false;
    eff_curve_save(true);
    CHECK(!s_curve_dirty);
}

static void test_an_unchanged_curve_is_not_rewritten(void)
{
    reset_curve();
    eff_bin_add(&s_curve.dis[3], 600.0f, 540.0f, 2000000);
    eff_curve_save(true);
    fake_nvs_fail = true;                       /* a write now would fail ... */
    eff_curve_save(true);
    CHECK(!s_curve_dirty);                      /* ... but none is attempted */
    fake_nvs_fail = false;
}

static void test_a_foreign_or_damaged_curve_is_discarded(void)
{
    reset_curve();
    eff_bin_add(&s_curve.dis[3], 600.0f, 540.0f, 2000000);
    s_curve.nbins = MB_EFF_BINS + 1;            /* written by another layout */
    eff_curve_save(true);
    memset(&s_curve, 0, sizeof(s_curve));
    eff_curve_load();
    CHECK_F(s_curve.dis[3].in_wh, 0, 0);

    reset_curve();
    eff_bin_add(&s_curve.dis[3], 600.0f, 540.0f, 2000000);
    s_curve.chg[2].out_wh = NAN;
    eff_curve_save(true);
    memset(&s_curve, 0, sizeof(s_curve));
    eff_curve_load();
    CHECK_F(s_curve.dis[3].in_wh, 0, 0);        /* all of it, not just the bad bin */
}

static void test_reset_empties_and_saves(void)
{
    reset_curve();
    eff_bin_add(&s_curve.dis[3], 600.0f, 540.0f, 2000000);
    eff_curve_save(true);
    modbus_rtu_reset_eff_curve();
    CHECK_I(s_curve.dis[3].s, 0);
    CHECK_I(s_curve.ver, MB_EFF_CURVE_VER);
    memset(&s_curve, 0xFF, sizeof(s_curve));
    eff_curve_load();
    CHECK_I(s_curve.dis[3].s, 0);               /* the empty curve is what was stored */
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
    RUN(test_slave_answers_a_valid_request);
    RUN(test_slave_ignores_what_is_not_for_it);
    RUN(test_stale_bridges_with_zero_then_goes_silent);
    RUN(test_hold_forever_keeps_the_old_behaviour);
    RUN(test_manipulation_expires_on_its_own);
    RUN(test_manipulation_is_off_after_a_restart);
    RUN(test_selftest_accepts_either_bus_as_master);
    RUN(test_selftest_refuses_without_a_partner);
    RUN(test_selftest_refuses_when_a_bus_is_switched_off);
    RUN(test_selftest_says_which_half_is_missing);
    RUN(test_hold_ramps_a_discharging_deye_to_zero);
    RUN(test_hold_still_takes_a_surplus);
    RUN(test_lock_holds_and_survives_the_managers_release);
    RUN(test_hold_never_touches_a_stale_reading);
    RUN(test_hold_expires_and_needs_fresh_output);
    RUN(test_bins_follow_the_edges);
    RUN(test_a_round_adds_energy_and_time);
    RUN(test_the_curve_survives_a_restart);
    RUN(test_a_failed_save_is_retried);
    RUN(test_an_unchanged_curve_is_not_rewritten);
    RUN(test_a_foreign_or_damaged_curve_is_discarded);
    RUN(test_reset_empties_and_saves);
    return t_report("modbus_rtu");
}

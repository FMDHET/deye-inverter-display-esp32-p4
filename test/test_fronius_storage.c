#include <stddef.h>
#include "harness.h"
#include "fakes.h"

/* The BYD behind the Fronius is the household's last-resort reserve. What
 * matters here is what the module WRITES to it: exactly the three registers
 * of a block, nothing without a request, the originals back on release, and
 * an error -- not "blocked" -- when the inverter acknowledges a write and keeps
 * its old value. A simulated Fronius with the register values read from the
 * real device (192.168.177.33, 2026-10) answers instead of the network. */
#define FRONIUS_STORAGE_HOST_TEST
#include "fronius_storage.c"

/* ------------------------- a simulated Fronius ------------------------- */

#define FBASE   40000
#define FN      400
#define M124    40303                 /* model start as found on the device */
#define P(f)    (M124 + 2 + (f))      /* payload register of field f        */

typedef struct {
    const char *ip;
    uint16_t    r[FN];
} fdev_t;

static fdev_t   f_dev[2];
static int      f_cur = -1;           /* open handle, -1 = none          */
static int      f_opens, f_closes, f_requests;
static int      f_broken;             /* closes that reported a failed transaction */
static bool     f_dead;               /* connect refused                 */
static bool     f_mute;               /* connects, never answers         */
static int      f_cut_after = -1;     /* requests answered, then silence */
static int      f_ignore_reg;         /* write acknowledged, not stored  */
static int      f_exc_reg;            /* write answered with exception 4 */
static int      f_foreign;            /* stale frames to send first      */
static uint8_t  f_out[2048];
static int      f_out_len, f_out_pos;

static int      f_nw;                 /* writes seen by the inverter     */
static uint16_t f_wreg[256], f_wval[256];

static mb_dev_cfg_t f_list[MB_MAX_DEVICES];
static int          f_nlist;

int modbus_tcp_get_devices(mb_dev_cfg_t *out, int max)
{
    int n = f_nlist < max ? f_nlist : max;
    memcpy(out, f_list, sizeof(mb_dev_cfg_t) * (size_t)n);
    return n;
}

static uint16_t *R(int dev, int reg) { return &f_dev[dev].r[reg - FBASE]; }

/* The register map as read from the real inverter. */
static void f_map_real(int dev)
{
    memset(f_dev[dev].r, 0, sizeof(f_dev[dev].r));
    *R(dev, 40000) = 0x5375; *R(dev, 40001) = 0x6E53;
    static const uint16_t chain[][3] = {
        { 40002,   1, 65 }, { 40069, 103, 50 }, { 40121, 120, 26 },
        { 40149, 121, 30 }, { 40181, 122, 44 }, { 40227, 123, 24 },
        { 40253, 160, 48 }, { 40303, 124, 24 }, { 40329, 0xFFFF, 0 },
    };
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        *R(dev, chain[i][0]) = chain[i][1]; *R(dev, chain[i][0] + 1) = chain[i][2];
    }
    static const uint16_t pl[24] = {
        7680, 100, 100, 3, 0xFFFF, 1200, 7920, 0xFFFF, 0xFFFF, 3,
        2604, 2604, 0, 65535, 0, 1,
        0, 0, 0x8000, (uint16_t)-2, (uint16_t)-2, 0x8000, 0x8000, (uint16_t)-2,
    };
    for (int i = 0; i < 24; i++) *R(dev, P(i)) = pl[i];
}

static void put(uint8_t **o, uint16_t v) { (*o)[0] = (uint8_t)(v >> 8); (*o)[1] = (uint8_t)v; *o += 2; }

static void f_reply(const uint8_t *pdu, int pdu_len, uint16_t tid, uint8_t unit)
{
    uint8_t *o = f_out + f_out_len;
    put(&o, tid); put(&o, 0); put(&o, (uint16_t)(pdu_len + 1));
    *o++ = unit;
    memcpy(o, pdu, (size_t)pdu_len);
    f_out_len += 7 + pdu_len;
}

static void f_exc(uint8_t fc, uint8_t code, uint16_t tid, uint8_t unit)
{
    uint8_t pdu[2] = { (uint8_t)(fc | 0x80), code };
    f_reply(pdu, 2, tid, unit);
}

static int f_open(const char *ip, uint16_t port, int timeout_ms)
{
    CHECK(f_cur < 0);                          /* one connection at a time */
    CHECK_I(timeout_ms, 3000);
    if (f_dead || port != 502) return -1;
    for (int i = 0; i < 2; i++)
        if (f_dev[i].ip && !strcmp(f_dev[i].ip, ip)) {
            f_cur = i; f_opens++; f_out_len = f_out_pos = 0;
            return i;
        }
    return -1;
}

static void f_close_h(int h, bool broken) { CHECK_I(h, f_cur); f_cur = -1; f_closes++; f_broken += broken; }

static int f_recv(int h, uint8_t *b, int n)
{
    if (h != f_cur || f_out_len - f_out_pos < n) return -1;   /* timeout */
    memcpy(b, f_out + f_out_pos, (size_t)n);
    f_out_pos += n;
    return 0;
}

static int f_send(int h, const uint8_t *b, int len)
{
    if (h != f_cur || len < 12) return -1;
    f_requests++;
    if (f_mute) return 0;
    if (f_cut_after >= 0 && f_requests > f_cut_after) return 0;
    if (f_out_pos == f_out_len) f_out_len = f_out_pos = 0;

    uint16_t tid = (uint16_t)((b[0] << 8) | b[1]);
    uint8_t  unit = b[6], fc = b[7];
    uint16_t reg = (uint16_t)((b[8] << 8) | b[9]), n = (uint16_t)((b[10] << 8) | b[11]);
    CHECK_I((b[4] << 8) | b[5], len - 6);

    if (f_foreign > 0) {                       /* a late answer from before */
        f_foreign--;
        uint8_t pdu[4] = { 0x03, 2, 0xAB, 0xCD };
        f_reply(pdu, 4, (uint16_t)(tid ^ 0x5555), unit);
    }
    if (reg < FBASE || reg + n > FBASE + FN || n == 0) { f_exc(fc, 2, tid, unit); return 0; }

    if (fc == 0x03) {
        uint8_t pdu[2 + 250], *o = pdu + 2;
        pdu[0] = 0x03; pdu[1] = (uint8_t)(2 * n);
        for (int i = 0; i < n; i++) put(&o, *R(h, reg + i));
        f_reply(pdu, 2 + 2 * n, tid, unit);
    } else if (fc == 0x10) {
        CHECK_I(b[12], 2 * n);
        CHECK_I(len, 13 + 2 * n);
        if (reg == f_exc_reg) { f_exc(fc, 4, tid, unit); return 0; }
        for (int i = 0; i < n; i++) {
            uint16_t v = (uint16_t)((b[13 + 2 * i] << 8) | b[14 + 2 * i]);
            if (f_nw < 256) { f_wreg[f_nw] = (uint16_t)(reg + i); f_wval[f_nw] = v; }
            f_nw++;
            if (reg + i != f_ignore_reg) *R(h, reg + i) = v;
        }
        uint8_t pdu[5], *o = pdu + 1;
        pdu[0] = 0x10; put(&o, reg); put(&o, n);
        f_reply(pdu, 5, tid, unit);
    } else {
        f_exc(fc, 1, tid, unit);
    }
    return 0;
}

static const fs_io_t f_io = { f_open, f_send, f_recv, f_close_h };

static void add_dev(const char *ip, uint8_t mfr, uint8_t role, bool en)
{
    mb_dev_cfg_t *d = &f_list[f_nlist++];
    memset(d, 0, sizeof(*d));
    snprintf(d->ip, sizeof(d->ip), "%s", ip);
    d->port = 502; d->slave = 1; d->mfr = mfr; d->role = role; d->enabled = en;
}

#define T0 1760000000LL

static void reset(void)
{
    /* module state */
    memset(&s_st, 0, sizeof(s_st));
    s_req_on = false; s_req_rvrt = 0; s_req_seq = 0;
    s_lock = NULL;
    memset(&s_dev, 0, sizeof(s_dev));
    s_owned = s_orig_known = s_active = false;
    s_orig_ctl = s_orig_out = s_orig_rvrt = 0;
    s_in_known = s_in_touched = false; s_orig_in = 0;
    s_applied = FS_M_NONE; s_applied_w = 0;
    memset(&s_tgt, 0, sizeof(s_tgt));
    memset(&s_man, 0, sizeof(s_man)); s_man_end[0] = 0;
    s_req_cap = 0; s_applied_cap = false; s_cap_want = false; s_req_lock = false;
    s_applied_rvrt = 0;
    s_last_read = s_last_refresh = 0;
    s_seen_seq = 0; s_writes = 0;
    s_no_revert = false; s_loaded = false;
    fake_nvs_reset_em();
    s_io = &f_io;
    /* the inverter */
    memset(f_dev, 0, sizeof(f_dev));
    f_dev[0].ip = "192.168.177.33"; f_map_real(0);
    f_cur = -1; f_opens = f_closes = f_requests = f_broken = 0;
    f_dead = f_mute = false; f_cut_after = -1;
    f_ignore_reg = f_exc_reg = 0; f_foreign = 0;
    f_out_len = f_out_pos = 0; f_nw = 0;
    f_nlist = 0;
    add_dev("192.168.177.33", MB_MFR_FRONIUS, MB_ROLE_INVERTER, true);
}

static fronius_storage_status_t st(void)
{
    fronius_storage_status_t s;
    fronius_storage_get(&s);
    return s;
}

/* ------------------------------- tests --------------------------------- */

static void test_frame_encode_decode(void)
{
    uint8_t b[64];
    CHECK_I(mbt_build_read(b, 0x1234, 1, 40303, 26), 12);
    static const uint8_t rd[12] = { 0x12, 0x34, 0, 0, 0, 6, 1, 3, 0x9D, 0x6F, 0, 26 };
    CHECK(memcmp(b, rd, 12) == 0);

    uint16_t v[2] = { 0x0708, 0xFFFF };
    CHECK_I(mbt_build_write(b, sizeof(b), 7, 1, 40315, 2, v), 17);
    static const uint8_t wr[17] = { 0, 7, 0, 0, 0, 11, 1, 0x10, 0x9D, 0x7B, 0, 2, 4,
                                    0x07, 0x08, 0xFF, 0xFF };
    CHECK(memcmp(b, wr, 17) == 0);
    CHECK_I(mbt_build_write(b, 14, 7, 1, 40315, 2, v), -1);       /* does not fit */
    CHECK_I(mbt_build_write(b, sizeof(b), 7, 1, 40315, 0, v), -1);

    /* read answer */
    uint16_t out[2] = { 0, 0 };
    uint8_t exc = 0;
    uint8_t ok[] = { 0x12, 0x34, 0, 0, 0, 7, 1, 3, 4, 0x1E, 0xF0, 0xFF, 0xFE };
    CHECK_I(mbt_parse_read(ok, sizeof(ok), 0x1234, 1, 2, out, &exc), FS_OK);
    CHECK_I(out[0], 7920); CHECK_I(out[1], 0xFFFE);
    CHECK_I(mbt_parse_read(ok, sizeof(ok), 0x1235, 1, 2, out, &exc), FS_E_FOREIGN);
    CHECK_I(mbt_parse_read(ok, sizeof(ok), 0x1234, 2, 2, out, &exc), FS_E_FOREIGN);
    CHECK_I(mbt_parse_read(ok, sizeof(ok), 0x1234, 1, 3, out, &exc), FS_E_FRAME);
    uint8_t badproto[sizeof(ok)]; memcpy(badproto, ok, sizeof(ok)); badproto[3] = 1;
    CHECK_I(mbt_parse_read(badproto, sizeof(ok), 0x1234, 1, 2, out, &exc), FS_E_FRAME);
    CHECK_I(mbt_parse_read(ok, sizeof(ok) - 1, 0x1234, 1, 2, out, &exc), FS_E_FRAME);
    uint8_t badcnt[sizeof(ok)]; memcpy(badcnt, ok, sizeof(ok)); badcnt[8] = 2;
    CHECK_I(mbt_parse_read(badcnt, sizeof(ok), 0x1234, 1, 2, out, &exc), FS_E_FRAME);

    /* exceptions */
    uint8_t ex[] = { 0x12, 0x34, 0, 0, 0, 3, 1, 0x83, 0x02 };
    CHECK_I(mbt_parse_read(ex, sizeof(ex), 0x1234, 1, 2, out, &exc), FS_E_EXC);
    CHECK_I(exc, 2);
    uint8_t exw[] = { 0x12, 0x34, 0, 0, 0, 3, 1, 0x90, 0x04 };
    CHECK_I(mbt_parse_write(exw, sizeof(exw), 0x1234, 1, 40315, 1, &exc), FS_E_EXC);
    CHECK_I(exc, 4);
    /* an exception for another function is not ours to interpret */
    CHECK_I(mbt_parse_write(ex, sizeof(ex), 0x1234, 1, 40315, 1, &exc), FS_E_FRAME);

    /* write echo */
    uint8_t we[] = { 0, 9, 0, 0, 0, 6, 1, 0x10, 0x9D, 0x7B, 0, 1 };
    CHECK_I(mbt_parse_write(we, sizeof(we), 9, 1, 40315, 1, &exc), FS_OK);
    CHECK_I(mbt_parse_write(we, sizeof(we), 9, 1, 40316, 1, &exc), FS_E_FRAME);
    CHECK_I(mbt_parse_write(we, sizeof(we), 9, 1, 40315, 2, &exc), FS_E_FRAME);
}

static void test_scaling(void)
{
    CHECK_F(ss_scale(7920, (uint16_t)-2, false), 79.2, 1e-4);
    CHECK_F(ss_scale(7680, 0, false), 7680, 1e-3);
    CHECK_F(ss_scale(2604, (uint16_t)-2, true), 26.04, 1e-4);
    CHECK_F(ss_scale((uint16_t)-500, (uint16_t)-2, true), -5.0, 1e-4);   /* int16 */
    CHECK_F(ss_scale(12, 2, false), 1200, 1e-3);
    CHECK(isnan(ss_scale(0xFFFF, 0, false)));      /* not implemented */
    CHECK(isnan(ss_scale(0x8000, 0, true)));
    CHECK(isnan(ss_scale(100, 0x8000, false)));    /* SF not implemented */
    CHECK(isnan(ss_scale(100, 40, false)));        /* absurd exponent */

    uint16_t r = 1;
    CHECK(ss_unscale(0.0f, (uint16_t)-2, true, &r));   CHECK_I(r, 0);
    CHECK(ss_unscale(100.0f, (uint16_t)-2, true, &r)); CHECK_I(r, 10000);
    CHECK(ss_unscale(15.0f, (uint16_t)-2, false, &r)); CHECK_I(r, 1500);
    CHECK(ss_unscale(-5.0f, (uint16_t)-2, true, &r));  CHECK_I((int16_t)r, -500);
    CHECK(!ss_unscale(400.0f, (uint16_t)-2, true, &r));   /* > int16 */
    CHECK(!ss_unscale(-1.0f, 0, false, &r));              /* < uint16 */
    CHECK(!ss_unscale(1.0f, 0x8000, false, &r));
    CHECK(!ss_unscale(NAN, 0, false, &r));
}

static void test_model_walk(void)
{
    reset();
    fs_conn_t c;
    CHECK_I(fs_open(&c, "192.168.177.33", 502, 1), FS_OK);
    uint16_t addr = 0;
    CHECK_I(fs_find_124(&c, &addr), FS_OK);
    CHECK_I(addr, 40303);
    fs_close(&c);

    /* the address follows the chain: a longer model 160 moves 124 */
    reset();
    f_map_real(0);
    *R(0, 40254) = 68;                          /* 160 with 3 MPPT */
    memset(R(0, 40303), 0, 2 * 26);
    *R(0, 40323) = 124; *R(0, 40324) = 24;
    *R(0, 40349) = 0xFFFF;                      /* end marker */
    CHECK_I(fs_open(&c, "192.168.177.33", 502, 1), FS_OK);
    CHECK_I(fs_find_124(&c, &addr), FS_OK);
    CHECK_I(addr, 40323);
    fs_close(&c);

    /* no storage model: the chain ends without 124 */
    reset();
    *R(0, 40303) = 125;
    CHECK_I(fs_open(&c, "192.168.177.33", 502, 1), FS_OK);
    CHECK_I(fs_find_124(&c, &addr), FS_E_NOTFOUND);
    fs_close(&c);
    fronius_storage_tick(T0);
    fronius_storage_status_t s = st();
    CHECK(!s.ok);
    CHECK_I(s.addr, 0);
    CHECK(strstr(s.err, "124") != NULL);
    CHECK_I(f_opens, f_closes);

    /* no SunSpec at all */
    reset();
    *R(0, 40000) = 0;
    fronius_storage_tick(T0);
    CHECK(!st().ok);
    CHECK(strstr(st().err, "124") != NULL);
    CHECK_I(f_nw, 0);
}

static void test_status_read(void)
{
    reset();
    fronius_storage_tick(T0);
    fronius_storage_status_t s = st();
    CHECK(s.ok);
    CHECK_I(s.addr, 40303);
    CHECK(!strcmp(s.ip, "192.168.177.33"));
    CHECK_F(s.soc_pct, 79.2, 1e-3);
    CHECK_F(s.reserve_pct, 12.0, 1e-3);
    CHECK_F(s.wchamax_w, 7680, 1e-2);
    CHECK_F(s.out_rte_pct, 26.04, 1e-3);
    CHECK_F(s.in_rte_pct, 26.04, 1e-3);
    CHECK_I(s.ctl_mod, 3);
    CHECK_I(s.rvrt_s, 65535);
    CHECK_I(s.cha_st, 3);
    CHECK_I(s.grid_charge, 1);
    CHECK(!s.blocked);
    CHECK_I(s.read_at, T0);
    CHECK(s.err[0] == 0);
    CHECK_I(f_opens, 1); CHECK_I(f_closes, 1);   /* borrowed for the batch */
    CHECK_I(f_broken, 0);                        /* and handed back intact */

    /* steady state: the cached address, one read per 30 s, no walk */
    int req = f_requests;
    fronius_storage_tick(T0 + 10);
    fronius_storage_tick(T0 + 20);
    CHECK_I(f_requests, req);                    /* not due yet */
    fronius_storage_tick(T0 + 30);
    CHECK_I(f_requests, req + 1);
    CHECK_I(st().read_at, T0 + 30);

    /* a foreign frame in the stream is stepped over */
    f_foreign = 1;
    *R(0, P(F_CHASTATE)) = 8000;
    fronius_storage_tick(T0 + 60);
    CHECK(st().ok);
    CHECK_F(st().soc_pct, 80.0, 1e-3);
    CHECK_I(f_broken, 0);                        /* stepped over, still in step */
}

static void test_no_write_without_request(void)
{
    reset();
    for (int t = 0; t < 7200; t += 10) fronius_storage_tick(T0 + t);
    CHECK_I(f_nw, 0);
    CHECK_I(s_writes, 0);
    CHECK(st().ok);
    CHECK_I(f_opens, 240);                       /* one per 30 s */
    CHECK_I(f_opens, f_closes);
    /* a release request with nothing of ours in force writes nothing either */
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 7200);
    CHECK_I(f_nw, 0);
}

static void test_block_on(void)
{
    reset();
    fronius_storage_tick(T0);
    CHECK_I(f_nw, 0);
    CHECK_I(fronius_storage_block_discharge(true, 1800), ESP_OK);
    CHECK_I(f_nw, 0);                            /* applied in the tick, not here */
    fronius_storage_tick(T0 + 1);                /* request -> at once */
    CHECK_I(f_nw, 3);
    CHECK_I(f_wreg[0], P(F_RVRTTMS));    CHECK_I(f_wval[0], 1800);
    CHECK_I(f_wreg[1], P(F_OUTWRTE));    CHECK_I(f_wval[1], 0);
    CHECK_I(f_wreg[2], P(F_STORCTL_MOD)); CHECK_I(f_wval[2], 3);
    fronius_storage_status_t s = st();
    CHECK(s.blocked);
    CHECK(s.err[0] == 0);
    CHECK_F(s.out_rte_pct, 0.0, 1e-6);
    CHECK_I(s.rvrt_s, 1800);
    CHECK_F(s.in_rte_pct, 26.04, 1e-3);          /* charge limit untouched */
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(f_opens, f_closes);

    /* bit0 is kept as found: no charge limit before -> none after */
    reset();
    *R(0, P(F_STORCTL_MOD)) = 0;
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK_I(f_nw, 3);
    CHECK_I(f_wval[2], 2);
    CHECK(st().blocked);

    /* the revert time is clamped: never "no revert", never hours on end */
    reset();
    fronius_storage_block_discharge(true, 5);
    fronius_storage_tick(T0);
    CHECK_I(f_wval[0], FRONIUS_STORAGE_RVRT_MIN_S);
    reset();
    fronius_storage_block_discharge(true, 65535);
    fronius_storage_tick(T0);
    CHECK_I(f_wval[0], FRONIUS_STORAGE_RVRT_MAX_S);
    reset();
    fronius_storage_block_discharge(true, 0);
    fronius_storage_tick(T0);
    CHECK_I(f_wval[0], FRONIUS_STORAGE_RVRT_DEF_S);
}

static void test_refresh_timing(void)
{
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK_I(f_nw, 3);
    int64_t last = T0;
    int refreshes = 0;
    for (int64_t t = T0 + 10; t <= T0 + 4 * 3600; t += 10) {
        int before = f_nw;
        fronius_storage_tick(t);
        if (f_nw != before) {
            CHECK_I(f_nw - before, 1);           /* only RvrtTms */
            CHECK_I(f_wreg[before], P(F_RVRTTMS));
            CHECK_I(f_wval[before], 1800);
            CHECK(t - last <= 1800 / 3);
            CHECK(t - last >= 1800 / 3);         /* and not more often */
            last = t;
            refreshes++;
        }
    }
    CHECK_I(refreshes, 24);
    CHECK(st().blocked);
    CHECK(st().err[0] == 0);
}

static void test_block_off_restores(void)
{
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    int n0 = f_nw;
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 5);
    CHECK_I(f_nw - n0, 3);
    CHECK_I(f_wreg[n0 + 0], P(F_OUTWRTE));     CHECK_I(f_wval[n0 + 0], 2604);
    CHECK_I(f_wreg[n0 + 1], P(F_STORCTL_MOD)); CHECK_I(f_wval[n0 + 1], 3);
    CHECK_I(f_wreg[n0 + 2], P(F_RVRTTMS));     CHECK_I(f_wval[n0 + 2], 65535);
    fronius_storage_status_t s = st();
    CHECK(!s.blocked);
    CHECK(s.err[0] == 0);
    CHECK_F(s.out_rte_pct, 26.04, 1e-3);
    CHECK_I(s.rvrt_s, 65535);
    CHECK(!s_owned);
    /* and nothing more after that */
    n0 = f_nw;
    for (int t = 10; t < 3600; t += 10) fronius_storage_tick(T0 + t);
    CHECK_I(f_nw, n0);

    /* a second block captures the originals again, not our 0 */
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0 + 4000);
    CHECK(st().blocked);
    CHECK_I(s_orig_out, 2604);
    CHECK_I(s_orig_rvrt, 65535);
}

static void test_ignored_write(void)
{
    /* the inverter says "ok" and keeps its old discharge limit */
    reset();
    f_ignore_reg = P(F_OUTWRTE);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    fronius_storage_status_t s = st();
    CHECK(!s.blocked);
    CHECK(s.ok);
    CHECK(strstr(s.err, "Nicht übernommen") != NULL);
    CHECK(strstr(s.err, "40315") != NULL);
    CHECK_F(s.out_rte_pct, 26.04, 1e-3);         /* status shows the truth */
    CHECK(s_owned);                               /* we still owe a restore */
    /* no write storm: the retry waits for the next read */
    int n0 = f_nw;
    fronius_storage_tick(T0 + 10);
    fronius_storage_tick(T0 + 20);
    CHECK_I(f_nw, n0);
    fronius_storage_tick(T0 + 30);
    CHECK_I(f_nw, n0 + 3);
    CHECK(!st().blocked);

    /* release puts back what was there, even after a failed block */
    f_ignore_reg = 0;
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 31);
    CHECK(!st().blocked);
    CHECK(st().err[0] == 0);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 3);
    CHECK_I(*R(0, P(F_RVRTTMS)), 65535);
    CHECK(!s_owned);

    /* ignored StorCtl_Mod and ignored RvrtTms are caught the same way */
    reset();
    *R(0, P(F_STORCTL_MOD)) = 1;
    f_ignore_reg = P(F_STORCTL_MOD);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(!st().blocked);
    CHECK(strstr(st().err, "40308") != NULL);
    /* RvrtTms ignored (stays 0xFFFF, SunSpec "not implemented") is NOT an
     * error: the Symo Hybrid 5.0-3-S does exactly that (2026-10-21). The
     * block stands, flagged as one that will not lift itself. */
    reset();
    f_ignore_reg = P(F_RVRTTMS);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    CHECK(st().no_revert);
    CHECK(st().err[0] == 0);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);
    /* an RvrtTms that keeps an old real value is still a failed write */
    reset();
    *R(0, P(F_RVRTTMS)) = 600;
    f_ignore_reg = P(F_RVRTTMS);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(!st().blocked);
    CHECK(strstr(st().err, "40318") != NULL);
}

/* No revert timer: no refresh writes, no reapply loop -- the 30-s read
 * only checks the block is still there. */
static void test_no_revert_device(void)
{
    reset();
    f_ignore_reg = P(F_RVRTTMS);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked && st().no_revert);
    int n0 = f_nw;
    for (int64_t t = T0 + 10; t < T0 + 3 * 3600; t += 10) fronius_storage_tick(t);
    CHECK_I(f_nw, n0);                            /* nothing written in 3 h */
    CHECK(st().blocked);

    /* the Fronius takes its limit back on its own: reapplied once */
    *R(0, P(F_OUTWRTE)) = 2604;
    fronius_storage_tick(T0 + 3 * 3600 + 30);
    CHECK(st().blocked);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);
}

/* The display restarts while our block is in the inverter. Without a revert
 * timer nobody else lifts it: the persisted state must, with the REAL
 * originals (not 100 %), and on the device it was set on. */
static void test_release_after_restart(void)
{
    reset();
    f_ignore_reg = P(F_RVRTTMS);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    CHECK(fronius_storage_owed());

    /* the restart: RAM gone, NVS (fake) kept */
    memset(&s_st, 0, sizeof(s_st));
    s_req_on = false; s_req_rvrt = 0; s_req_seq = 0;
    memset(&s_dev, 0, sizeof(s_dev));
    s_owned = s_orig_known = s_active = false;
    s_orig_ctl = s_orig_out = s_orig_rvrt = 0;
    s_in_known = s_in_touched = false; s_orig_in = 0;
    memset(&s_man, 0, sizeof(s_man));
    s_no_revert = false; s_loaded = false;
    s_last_read = s_last_refresh = 0; s_seen_seq = 0;

    CHECK(fronius_storage_owed());                /* known again from NVS */
    fronius_storage_tick(T0 + 100);               /* nothing requested: release */
    CHECK(!st().blocked);
    CHECK(st().err[0] == 0);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);           /* the operator's 2 kW, not 100 % */
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 3);
    CHECK(!fronius_storage_owed());

    /* and after that restart again: nothing owed, nothing written */
    s_loaded = false; s_owned = false;
    int n0 = f_nw;
    fronius_storage_tick(T0 + 200);
    CHECK(!fronius_storage_owed());
    CHECK_I(f_nw, n0);
}

static void test_write_exception(void)
{
    reset();
    f_exc_reg = P(F_STORCTL_MOD);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(!st().blocked);
    CHECK(strstr(st().err, "Ausnahme 4") != NULL);
    CHECK_I(f_nw, 2);                            /* stopped at the failing write */
}

static void test_dead_connection(void)
{
    /* connect refused */
    reset();
    f_dead = true;
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    fronius_storage_status_t s = st();
    CHECK(!s.ok);
    CHECK(!s.blocked);
    CHECK(strstr(s.err, "Keine Verbindung") != NULL);
    CHECK_I(f_nw, 0);
    CHECK_I(fronius_storage_set_reserve(20.0f), ESP_FAIL);

    /* connects, never answers */
    reset();
    f_mute = true;
    fronius_storage_tick(T0);
    CHECK(!st().ok);
    CHECK(strstr(st().err, "Keine Antwort") != NULL);
    CHECK_I(f_opens, f_closes);
    /* the poll's connection goes back marked: an answer may still come */
    CHECK_I(f_broken, f_closes);

    /* dies in the middle of a block: error, rediscovery, restore later */
    reset();
    fronius_storage_tick(T0);
    CHECK(st().ok);
    fronius_storage_block_discharge(true, 1800);
    f_cut_after = f_requests + 2;               /* read + first write answered */
    fronius_storage_tick(T0 + 1);
    CHECK(st().ok);                              /* the read itself worked */
    CHECK(!st().blocked);
    CHECK(strstr(st().err, "Keine Antwort") != NULL);
    CHECK_I(s_dev.addr, 0);                      /* walk again next time */
    CHECK(s_owned);
    CHECK_I(f_opens, f_closes);
    /* the inverter comes back: block completes */
    f_cut_after = -1;
    fronius_storage_tick(T0 + 31);
    CHECK(st().ok);
    CHECK(st().blocked);
    CHECK_I(s_orig_rvrt, 65535);                 /* the first capture, not 1800 */

    /* dies while blocked: the refresh fails, nothing crashes, the inverter's
     * own timer is the safety net; back again -> block re-applied */
    f_dead = true;
    for (int t = 60; t <= 700; t += 10) fronius_storage_tick(T0 + t);
    CHECK(!st().blocked);
    CHECK(!st().ok);
    f_dead = false;
    fronius_storage_tick(T0 + 730);
    CHECK(st().blocked);
}

static void test_drift_reapplied(void)
{
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    /* the Fronius takes its own limit back (revert ran out / own management) */
    *R(0, P(F_OUTWRTE)) = 2604;
    *R(0, P(F_RVRTTMS)) = 65535;
    int n0 = f_nw;
    fronius_storage_tick(T0 + 30);
    CHECK_I(f_nw - n0, 3);
    CHECK(st().blocked);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);
    CHECK_I(s_orig_out, 2604);
    /* a block whose timer was reset to "never" counts as not intact */
    *R(0, P(F_RVRTTMS)) = 65535;
    n0 = f_nw;
    fronius_storage_tick(T0 + 60);
    CHECK_I(f_nw - n0, 3);
    CHECK_I(*R(0, P(F_RVRTTMS)), 1800);
}

static void test_leftover_after_reboot(void)
{
    /* the display restarted inside a block: the inverter still shows our 0 */
    reset();
    *R(0, P(F_OUTWRTE)) = 0;
    *R(0, P(F_RVRTTMS)) = 1200;
    fronius_storage_tick(T0);
    CHECK_I(f_nw, 0);                            /* never touched unasked */
    CHECK(!st().blocked);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0 + 1);
    CHECK(st().blocked);
    CHECK(!s_orig_known);
    int n0 = f_nw;
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 2);
    CHECK_I(f_nw - n0, 2);                       /* RvrtTms stays running */
    CHECK_I(f_wreg[n0], P(F_OUTWRTE));      CHECK_I(f_wval[n0], 10000);
    CHECK_I(f_wreg[n0 + 1], P(F_STORCTL_MOD)); CHECK_I(f_wval[n0 + 1], 1);
    CHECK(!st().blocked);
    CHECK(!s_owned);
}

static void test_set_reserve(void)
{
    reset();
    CHECK_I(fronius_storage_set_reserve(15.0f), ESP_ERR_INVALID_STATE);  /* no tick yet */
    fronius_storage_tick(T0);
    CHECK_I(fronius_storage_set_reserve(-1.0f), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set_reserve(100.5f), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set_reserve(NAN), ESP_ERR_INVALID_ARG);
    CHECK_I(f_nw, 0);
    CHECK_I(fronius_storage_set_reserve(15.0f), ESP_OK);
    CHECK_I(f_nw, 1);
    CHECK_I(f_wreg[0], P(F_MINRSVPCT)); CHECK_I(f_wval[0], 1500);
    CHECK_F(st().reserve_pct, 15.0, 1e-3);
    CHECK(st().err[0] == 0);
    CHECK_I(f_opens, f_closes);

    f_ignore_reg = P(F_MINRSVPCT);
    CHECK_I(fronius_storage_set_reserve(20.0f), ESP_FAIL);
    CHECK_F(st().reserve_pct, 15.0, 1e-3);
    CHECK(strstr(st().err, "Nicht übernommen") != NULL);
}

static void test_device_choice(void)
{
    /* nothing configured */
    reset();
    f_nlist = 0;
    fronius_storage_tick(T0);
    CHECK(!st().ok);
    CHECK(strstr(st().err, "Kein Fronius") != NULL);
    CHECK_I(f_opens, 0);

    /* wrong role, disabled, other maker: skipped. A Fronius without 124
     * (plain string inverter): tried, passed over. */
    reset();
    f_nlist = 0;
    f_dev[1].ip = "192.168.177.34"; f_map_real(1); *R(1, 40303) = 125;
    add_dev("192.168.177.50", MB_MFR_FRONIUS, MB_ROLE_GRID, true);
    add_dev("192.168.177.51", MB_MFR_FRONIUS, MB_ROLE_INVERTER, false);
    add_dev("192.168.177.52", MB_MFR_DEYE, MB_ROLE_INVERTER, true);
    add_dev("192.168.177.34", MB_MFR_FRONIUS, MB_ROLE_INVERTER, true);
    add_dev("192.168.177.33", MB_MFR_FRONIUS, MB_ROLE_INVERTER, true);
    fronius_storage_tick(T0);
    CHECK(st().ok);
    CHECK(!strcmp(st().ip, "192.168.177.33"));
    CHECK_I(f_opens, 2);

    /* removed from the settings -> forgotten (nothing of ours in force) */
    f_nlist = 0;
    fronius_storage_tick(T0 + 30);
    CHECK(!st().ok);
    CHECK(strstr(st().err, "Kein Fronius") != NULL);

    /* ...but a device that still carries our block is kept for the restore */
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    f_nlist = 0;
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 1);
    CHECK(!s_owned);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
}

/* ------------------------- forced charge/discharge ---------------------- */

#define NEG(v) ((uint16_t)(int16_t)-(v))

/* The writes since n0 are exactly these (register, value) pairs, in order. */
static void expect_writes(int n0, int n, const uint16_t (*w)[2])
{
    CHECK_I(f_nw - n0, n);
    for (int i = 0; i < n && n0 + i < f_nw; i++) {
        CHECK_I(f_wreg[n0 + i], w[i][0]);
        CHECK_I(f_wval[n0 + i], w[i][1]);
    }
}

/* Restart of the display: RAM gone, NVS (fake) kept. */
static void restart(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_req_on = false; s_req_rvrt = 0; s_req_seq = 0;
    memset(&s_dev, 0, sizeof(s_dev));
    s_owned = s_orig_known = s_active = false;
    s_orig_ctl = s_orig_out = s_orig_rvrt = 0;
    s_in_known = s_in_touched = false; s_orig_in = 0;
    s_applied = FS_M_NONE; s_applied_w = 0;
    memset(&s_tgt, 0, sizeof(s_tgt));
    memset(&s_man, 0, sizeof(s_man)); s_man_end[0] = 0;
    s_req_cap = 0; s_applied_cap = false; s_cap_want = false; s_req_lock = false;
    s_no_revert = false; s_loaded = false;
    s_last_read = s_last_refresh = 0; s_seen_seq = 0;
}

static void test_force_charge(void)
{
    reset();
    fronius_storage_tick(T0);
    CHECK_I(f_nw, 0);
    /* 3000 W of WChaMax 7680 = 39.0625 % -> 3906 at scale -2 */
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 3000, 30, 90, T0), ESP_OK);
    CHECK_I(f_nw, 0);                             /* applied in the tick, not here */
    CHECK(fronius_storage_owed());                /* ...so the manager keeps ticking */
    fronius_storage_tick(T0 + 1);
    /* timer first; the charge limit opens (2604 -> 3906) before the discharge
     * side closes (2604 -> -3906); the bits last */
    static const uint16_t w1[][2] = {
        { P(F_RVRTTMS), FS_FORCE_RVRT_S }, { P(F_INWRTE), 3906 },
        { P(F_OUTWRTE), NEG(3906) },       { P(F_STORCTL_MOD), 3 },
    };
    expect_writes(0, 4, w1);
    fronius_storage_status_t s = st();
    CHECK(s.forcing);
    CHECK(!s.blocked);
    CHECK(s.err[0] == 0);
    CHECK_I(s.force, FRONIUS_FORCE_CHARGE);
    CHECK_I(s.force_w, 3000);
    CHECK_I(s.force_until, T0 + 30 * 60);
    CHECK_F(s.in_rte_pct, 39.06, 1e-3);
    CHECK_F(s.out_rte_pct, -39.06, 1e-3);
    CHECK(s.owed);
    CHECK(!fronius_storage_rollback_safe());       /* InWRte is ours now */
    CHECK_I(s_orig_in, 2604);

    /* nothing more while it runs (only the 600-s timer every 200 s) */
    int n0 = f_nw;
    for (int64_t t = T0 + 10; t < T0 + 30 * 60 - 10; t += 10) fronius_storage_tick(t);
    for (int i = n0; i < f_nw; i++) CHECK_I(f_wreg[i], P(F_RVRTTMS));
    CHECK(st().forcing);

    /* the ESP's own time limit: everything back as the operator had it,
     * the discharge side opens first */
    n0 = f_nw;
    fronius_storage_tick(T0 + 30 * 60);
    static const uint16_t w2[][2] = {
        { P(F_OUTWRTE), 2604 }, { P(F_INWRTE), 2604 },
        { P(F_STORCTL_MOD), 3 }, { P(F_RVRTTMS), 65535 },
    };
    expect_writes(n0, 4, w2);
    s = st();
    CHECK(!s.forcing && !s.owed);
    CHECK_I(s.force, FRONIUS_FORCE_OFF);
    CHECK(strcmp(s.force_end, "Zeit abgelaufen") == 0);
    CHECK(fronius_storage_rollback_safe());
    CHECK(!fronius_storage_owed());
    n0 = f_nw;
    for (int t = 10; t < 3600; t += 10) fronius_storage_tick(T0 + 30 * 60 + t);
    CHECK_I(f_nw, n0);
}

static void test_force_discharge(void)
{
    reset();
    fronius_storage_force(FRONIUS_FORCE_DISCHARGE, 3000, 60, 20, T0);
    fronius_storage_tick(T0);
    /* the discharge limit opens before the charge side closes */
    static const uint16_t w1[][2] = {
        { P(F_RVRTTMS), FS_FORCE_RVRT_S }, { P(F_OUTWRTE), 3906 },
        { P(F_INWRTE), NEG(3906) },        { P(F_STORCTL_MOD), 3 },
    };
    expect_writes(0, 4, w1);
    CHECK(st().forcing);

    /* by hand: off */
    int n0 = f_nw;
    fronius_storage_force(FRONIUS_FORCE_OFF, 0, 0, 0, T0 + 50);
    fronius_storage_tick(T0 + 51);
    static const uint16_t w2[][2] = {
        { P(F_INWRTE), 2604 }, { P(F_OUTWRTE), 2604 },
        { P(F_STORCTL_MOD), 3 }, { P(F_RVRTTMS), 65535 },
    };
    expect_writes(n0, 4, w2);
    CHECK(strcmp(st().force_end, "von Hand beendet") == 0);
    CHECK(!st().owed);

    /* more than the inverter can: clamped to WChaMax = 100 % */
    reset();
    fronius_storage_force(FRONIUS_FORCE_DISCHARGE, 9000, 10, 0, T0);
    fronius_storage_tick(T0);
    CHECK_I(*R(0, P(F_OUTWRTE)), 10000);
    CHECK_I(*R(0, P(F_INWRTE)), NEG(10000));
}

static void test_force_soc_end(void)
{
    /* the BYD reads 79.2 %: a charge to 50 % is over before it begins */
    reset();
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 2000, 60, 50, T0);
    fronius_storage_tick(T0);
    CHECK_I(f_nw, 0);
    CHECK(strcmp(st().force_end, "Ziel-SoC erreicht") == 0);
    CHECK(!fronius_storage_owed());

    /* to 90 %: runs until the SoC is there */
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 2000, 60, 90, T0 + 10);
    fronius_storage_tick(T0 + 10);
    CHECK(st().forcing);
    *R(0, P(F_CHASTATE)) = 8990;
    fronius_storage_tick(T0 + 40);
    CHECK(st().forcing);
    *R(0, P(F_CHASTATE)) = 9000;
    fronius_storage_tick(T0 + 70);
    CHECK(!st().forcing && !st().owed);
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);

    /* a discharge stops at or below its SoC */
    reset();
    *R(0, P(F_CHASTATE)) = 2100;
    fronius_storage_force(FRONIUS_FORCE_DISCHARGE, 2000, 60, 20, T0);
    fronius_storage_tick(T0);
    CHECK(st().forcing);
    *R(0, P(F_CHASTATE)) = 2000;
    fronius_storage_tick(T0 + 30);
    CHECK(!st().forcing && !st().owed);
}

/* The operator's forced mode wins over the manager's block; when it ends the
 * block comes back (with InWRte put back), and a release later restores all. */
static void test_force_over_block(void)
{
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 3000, 30, 100, T0 + 5);
    fronius_storage_tick(T0 + 5);
    CHECK(st().forcing && !st().blocked);
    CHECK_I(*R(0, P(F_INWRTE)), 3906);
    CHECK_I(*R(0, P(F_OUTWRTE)), NEG(3906));
    CHECK_I(s_orig_out, 2604);                    /* not the block's 0 */

    int n0 = f_nw;
    fronius_storage_force(FRONIUS_FORCE_OFF, 0, 0, 0, T0 + 10);
    fronius_storage_tick(T0 + 10);
    static const uint16_t w1[][2] = {
        { P(F_RVRTTMS), 1800 }, { P(F_OUTWRTE), 0 },
        { P(F_INWRTE), 2604 },  { P(F_STORCTL_MOD), 3 },
    };
    expect_writes(n0, 4, w1);
    CHECK(st().blocked && !st().forcing);

    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 20);
    CHECK(!st().owed);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 3);
    CHECK_I(*R(0, P(F_RVRTTMS)), 65535);
}

/* A forced mode does not survive a restart of the display: the persisted
 * originals (InWRte included) are put back. */
static void test_force_restart_releases(void)
{
    reset();
    f_ignore_reg = P(F_RVRTTMS);                  /* like the real device */
    fronius_storage_force(FRONIUS_FORCE_DISCHARGE, 2500, 120, 15, T0);
    fronius_storage_tick(T0);
    CHECK(st().forcing && st().no_revert);
    restart();
    CHECK(fronius_storage_owed());
    CHECK(!fronius_storage_rollback_safe());
    fronius_storage_tick(T0 + 100);
    CHECK(!st().forcing && !st().owed);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 3);
    CHECK(fronius_storage_rollback_safe());

    /* without a revert timer the time limit is still kept */
    reset();
    f_ignore_reg = P(F_RVRTTMS);
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 2500, 5, 100, T0);
    fronius_storage_tick(T0);
    CHECK(st().forcing);
    for (int64_t t = T0 + 10; t < T0 + 300; t += 10) fronius_storage_tick(t);
    CHECK(st().forcing);
    fronius_storage_tick(T0 + 300);
    CHECK(!st().forcing && !st().owed);

    /* a clock step backwards does not stretch it */
    reset();
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 2500, 5, 100, T0 + 100000);
    fronius_storage_tick(T0);
    CHECK(!st().forcing);
    CHECK(strcmp(st().force_end, "Zeit abgelaufen") == 0);
}

/* A block record written by the firmware before forced modes (no InWRte
 * fields) is read, released correctly, and a forced mode on top of it takes
 * InWRte's original from the inverter. */
static void test_force_old_record(void)
{
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    fs_rec_t r;
    CHECK_I(nvs_store_get_fs_state(&r, sizeof(r)), ESP_OK);
    nvs_store_set_fs_state(&r, offsetof(fs_rec_t, in));   /* the old length */
    restart();
    CHECK(fronius_storage_owed());
    CHECK(fronius_storage_rollback_safe());
    CHECK(!s_in_known);
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 3000, 30, 100, T0 + 10);
    fronius_storage_tick(T0 + 10);
    CHECK(st().forcing);
    CHECK(s_in_known);
    CHECK_I(s_orig_in, 2604);
    fronius_storage_force(FRONIUS_FORCE_OFF, 0, 0, 0, T0 + 20);
    fronius_storage_tick(T0 + 20);
    CHECK(!st().owed);
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
}

static void test_force_args(void)
{
    reset();
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 50, 10, 100, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 20000, 10, 100, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 1000, FRONIUS_FORCE_MAX_MIN + 1, 100, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 1000, 10, 101, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 1000, 10, NAN, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_force((fronius_force_t)7, 1000, 10, 50, T0), ESP_ERR_INVALID_ARG);
    CHECK_I(st().force, FRONIUS_FORCE_OFF);
    CHECK_I(fronius_storage_force(FRONIUS_FORCE_CHARGE, 1000, 0, 100, T0), ESP_OK);
    CHECK_I(st().force_until, T0 + FRONIUS_FORCE_DEF_MIN * 60);
    fronius_storage_tick(T0);
    CHECK_I(f_opens, f_closes);
}

/* The inverter acknowledges, keeps its old InWRte: an error, not "forcing". */
static void test_force_ignored(void)
{
    reset();
    f_ignore_reg = P(F_INWRTE);
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 3000, 30, 100, T0);
    fronius_storage_tick(T0);
    CHECK(!st().forcing);
    CHECK(strstr(st().err, "Nicht übernommen") != NULL);
    CHECK_I(f_nw, 2);                             /* timer, InWRte -- and stopped there: */
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);           /* the discharge side never closed */
    CHECK(st().owed);                             /* the half that went in comes back out */
    fronius_storage_force(FRONIUS_FORCE_OFF, 0, 0, 0, T0 + 5);
    f_ignore_reg = 0;
    fronius_storage_tick(T0 + 5);
    CHECK(!st().owed);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
}

/* ------------------------- settings by hand --------------------------- */

static void test_set_by_hand(void)
{
    reset();
    bool base = true;
    CHECK_I(fronius_storage_set(FRONIUS_REG_OUT, 50.0f, &base), ESP_ERR_INVALID_STATE);  /* no tick yet */
    fronius_storage_tick(T0);

    /* Nothing of ours in force: straight to the inverter, read back. */
    CHECK_I(fronius_storage_set(FRONIUS_REG_OUT, 50.0f, &base), ESP_OK);
    CHECK(!base);
    CHECK_I(*R(0, P(F_OUTWRTE)), 5000);
    CHECK_I(fronius_storage_set(FRONIUS_REG_IN, 12.5f, &base), ESP_OK);
    CHECK_I(*R(0, P(F_INWRTE)), 1250);
    CHECK_I(fronius_storage_set(FRONIUS_REG_RESERVE, 10.0f, NULL), ESP_OK);
    CHECK_I(*R(0, P(F_MINRSVPCT)), 1000);
    CHECK_I(fronius_storage_set(FRONIUS_REG_CTL, 1.0f, NULL), ESP_OK);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 1);
    CHECK_I(fronius_storage_set(FRONIUS_REG_GRID, 0.0f, NULL), ESP_OK);
    CHECK_I(*R(0, P(F_CHAGRISET)), 0);
    CHECK(!fronius_storage_owed());
    CHECK_F(st().out_rte_pct, 50.0, 1e-3);

    /* Out of range: refused before anything is sent. */
    int n0 = f_nw;
    CHECK_I(fronius_storage_set(FRONIUS_REG_OUT, -10.0f, NULL), ESP_ERR_INVALID_ARG);   /* that is forcing */
    CHECK_I(fronius_storage_set(FRONIUS_REG_OUT, 100.5f, NULL), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set(FRONIUS_REG_CTL, 4.0f, NULL), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set(FRONIUS_REG_CTL, 1.5f, NULL), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set(FRONIUS_REG_GRID, 2.0f, NULL), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set(FRONIUS_REG_RESERVE, NAN, NULL), ESP_ERR_INVALID_ARG);
    CHECK_I(fronius_storage_set((fronius_reg_t)9, 1.0f, NULL), ESP_ERR_INVALID_ARG);
    CHECK_I(f_nw, n0);

    /* An ignored write is an error, not a success. */
    f_ignore_reg = P(F_INWRTE);
    CHECK_I(fronius_storage_set(FRONIUS_REG_IN, 40.0f, NULL), ESP_FAIL);
    CHECK(strstr(st().err, "Nicht übernommen") != NULL);
    f_ignore_reg = 0;
}

/* While the block is in force, OutWRte and StorCtl_Mod are ours: a slider
 * changes the operator's BASE value, put back when the block ends. */
static void test_set_by_hand_during_a_block(void)
{
    reset();
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(st().blocked);
    CHECK(st().base_known);
    CHECK_I(st().base_out, 2604);

    bool base = false;
    int n0 = f_nw;
    CHECK_I(fronius_storage_set(FRONIUS_REG_OUT, 50.0f, &base), ESP_OK);
    CHECK(base);
    CHECK_I(f_nw, n0);                            /* the block stays in the inverter */
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);
    CHECK_I(st().base_out, 5000);

    /* StorCtl_Mod: the block keeps bit1, the base's bit0 comes through at
     * the next tick. */
    CHECK_I(fronius_storage_set(FRONIUS_REG_CTL, 0.0f, &base), ESP_OK);
    CHECK(base);
    fronius_storage_tick(T0 + 10);
    CHECK(st().blocked);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 2);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);

    /* InWRte is not the block's: live, and the base follows. */
    CHECK_I(fronius_storage_set(FRONIUS_REG_IN, 30.0f, &base), ESP_OK);
    CHECK(!base);
    CHECK_I(*R(0, P(F_INWRTE)), 3000);

    /* Release: the new base values, not the ones from before the block. */
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 20);
    CHECK(!st().owed);
    CHECK_I(*R(0, P(F_OUTWRTE)), 5000);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 0);
    CHECK_I(*R(0, P(F_INWRTE)), 3000);

    /* ... and the base survives a restart of the display. */
    reset();
    f_ignore_reg = P(F_RVRTTMS);
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    fronius_storage_set(FRONIUS_REG_OUT, 40.0f, NULL);
    restart();
    fronius_storage_tick(T0 + 100);               /* nothing requested: release */
    CHECK_I(*R(0, P(F_OUTWRTE)), 4000);

    /* Originals unknown (our 0 found after a reboot): no base to change. */
    reset();
    *R(0, P(F_OUTWRTE)) = 0;
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0);
    CHECK(!st().base_known);
    CHECK_I(fronius_storage_set(FRONIUS_REG_OUT, 40.0f, NULL), ESP_FAIL);
    CHECK(strstr(st().err, "Ursprungswerte unbekannt") != NULL);
}

/* --------------------------- upper charge cap --------------------------- */

static void test_charge_cap(void)
{
    reset();
    fronius_storage_set_charge_cap(80);
    CHECK(fronius_storage_owed());                /* the manager keeps ticking */
    fronius_storage_tick(T0);                     /* SoC 79.2 %: below the cap */
    CHECK_I(f_nw, 0);
    CHECK(!st().cap_on);
    CHECK_I(st().cap_pct, 80);

    *R(0, P(F_CHASTATE)) = 8000;                  /* 80 %: charging held */
    fronius_storage_tick(T0 + 30);
    CHECK(st().cap_on);
    CHECK(!st().blocked);
    CHECK_I(*R(0, P(F_INWRTE)), 0);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);           /* discharging as the operator had it */
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 3);
    CHECK_I(*R(0, P(F_RVRTTMS)), FRONIUS_STORAGE_RVRT_DEF_S);   /* never "no revert" */

    /* Hysteresis: 79 % stays held, below 78 % it is released. */
    *R(0, P(F_CHASTATE)) = 7900;
    int n0 = f_nw;
    fronius_storage_tick(T0 + 60);
    CHECK(st().cap_on);
    CHECK_I(f_nw, n0);
    *R(0, P(F_CHASTATE)) = 7790;
    fronius_storage_tick(T0 + 90);
    CHECK(!st().cap_on);
    CHECK(!st().owed);
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(*R(0, P(F_RVRTTMS)), 65535);

    /* Cap and the manager's block together: both rates held; the block
     * ends, the cap stays. */
    *R(0, P(F_CHASTATE)) = 8500;
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0 + 120);
    CHECK(st().blocked && st().cap_on);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);
    CHECK_I(*R(0, P(F_INWRTE)), 0);
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 130);
    CHECK(!st().blocked && st().cap_on);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
    CHECK_I(*R(0, P(F_INWRTE)), 0);

    /* The cap is general: a forced charge above it is over at once ... */
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 2000, 30, 95, T0 + 140);
    fronius_storage_tick(T0 + 140);
    CHECK(!st().forcing && st().cap_on);
    CHECK(strcmp(st().force_end, "Ladegrenze erreicht") == 0);
    CHECK_I(*R(0, P(F_INWRTE)), 0);

    /* ... and one from below runs only up to it. */
    fronius_storage_set_charge_cap(90);
    *R(0, P(F_CHASTATE)) = 8500;
    fronius_storage_tick(T0 + 145);               /* 85 %: under 90 - hyst */
    CHECK(!st().cap_on);
    fronius_storage_force(FRONIUS_FORCE_CHARGE, 2000, 30, 95, T0 + 146);
    fronius_storage_tick(T0 + 146);
    CHECK(st().forcing && !st().cap_on);
    CHECK(*R(0, P(F_INWRTE)) > 2000);
    *R(0, P(F_CHASTATE)) = 9000;
    fronius_storage_tick(T0 + 200);               /* next read */
    CHECK(!st().forcing);
    CHECK(strcmp(st().force_end, "Ladegrenze erreicht") == 0);
    CHECK(st().cap_on);                           /* held at the cap */
    CHECK_I(*R(0, P(F_INWRTE)), 0);
    fronius_storage_set_charge_cap(80);

    /* Cap off: everything back, nothing owed. */
    fronius_storage_set_charge_cap(0);
    fronius_storage_tick(T0 + 210);
    CHECK(!st().cap_on);
    CHECK(!st().owed);
    CHECK(!fronius_storage_owed());
    CHECK_I(*R(0, P(F_INWRTE)), 2604);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
    CHECK_I(*R(0, P(F_STORCTL_MOD)), 3);

    /* 100 % is off as well. */
    fronius_storage_set_charge_cap(100);
    CHECK_I(st().cap_pct, 0);
}

/* The operator's lock next to the manager's block: either blocks, neither
 * lifts the other's. */
static void test_lock_by_hand(void)
{
    reset();
    f_ignore_reg = P(F_RVRTTMS);                  /* like the real device */
    fronius_storage_lock_discharge(true);
    CHECK(fronius_storage_owed());
    fronius_storage_tick(T0);
    CHECK(st().blocked && st().lock_req);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);

    /* the manager blocks and lets go again: the lock stays */
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_tick(T0 + 10);
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 20);
    CHECK(st().blocked);
    CHECK_I(*R(0, P(F_OUTWRTE)), 0);

    /* the lock goes, the manager blocks: still blocked */
    fronius_storage_block_discharge(true, 1800);
    fronius_storage_lock_discharge(false);
    fronius_storage_tick(T0 + 30);
    CHECK(st().blocked && !st().lock_req);

    /* both gone: everything back */
    fronius_storage_block_discharge(false, 0);
    fronius_storage_tick(T0 + 40);
    CHECK(!st().blocked && !st().owed);
    CHECK_I(*R(0, P(F_OUTWRTE)), 2604);
}

int main(void)
{
    RUN(test_frame_encode_decode);
    RUN(test_scaling);
    RUN(test_model_walk);
    RUN(test_status_read);
    RUN(test_no_write_without_request);
    RUN(test_block_on);
    RUN(test_refresh_timing);
    RUN(test_block_off_restores);
    RUN(test_ignored_write);
    RUN(test_no_revert_device);
    RUN(test_release_after_restart);
    RUN(test_write_exception);
    RUN(test_dead_connection);
    RUN(test_drift_reapplied);
    RUN(test_leftover_after_reboot);
    RUN(test_set_reserve);
    RUN(test_device_choice);
    RUN(test_force_charge);
    RUN(test_force_discharge);
    RUN(test_force_soc_end);
    RUN(test_force_over_block);
    RUN(test_force_restart_releases);
    RUN(test_force_old_record);
    RUN(test_force_args);
    RUN(test_force_ignored);
    RUN(test_set_by_hand);
    RUN(test_set_by_hand_during_a_block);
    RUN(test_charge_cap);
    RUN(test_lock_by_hand);
    return t_report("fronius_storage");
}

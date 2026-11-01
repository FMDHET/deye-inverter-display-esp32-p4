#include "fronius_storage.h"
#include "nvs_store.h"
#include "modbus_tcp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fr_stor";

/* ------------------------------ constants ------------------------------ */

#define FS_TIMEOUT_MS      3000   /* connect and each read/write (host fake) */
#define FS_BORROW_WAIT_MS  8000   /* the poll may be mid-round on that address */
#define FS_READ_EVERY_S    30     /* status poll period                     */
#define FS_LOCK_WAIT_MS    15000  /* tick waits this long for set_reserve   */
#define FS_RESYNC          4      /* foreign frames skipped per request     */
#define FS_WALK_MAX        64     /* models walked before giving up         */
#define FS_FORCE_RVRT_S    600    /* dead-man revert for a forced mode, where the device has one */
#define FS_CAP_HYST        2.0f   /* charge cap: off again this many points below */

#define SUNS_HI            0x5375 /* "Su" */
#define SUNS_LO            0x6E53 /* "nS" */
#define M124_ID            124
#define M124_LEN           24     /* payload registers                       */
#define M124_REGS          (2 + M124_LEN)   /* header + payload, one read   */

#define CTL_CHARGE_LIM     0x0001 /* StorCtl_Mod bit0: InWRte in force      */
#define CTL_DISCHARGE_LIM  0x0002 /* StorCtl_Mod bit1: OutWRte in force     */

/* SunSpec "not implemented" markers. */
#define SS_NI_U16          0xFFFF
#define SS_NI_I16          0x8000

/* Model 124 payload layout (offsets from the first payload register, i.e.
 * model start + 2). Order as read from the device and as in the SunSpec spec. */
enum {
    F_WCHAMAX = 0, F_WCHAGRA, F_WDISCHAGRA, F_STORCTL_MOD, F_VACHAMAX,
    F_MINRSVPCT, F_CHASTATE, F_STORAVAL, F_INBATV, F_CHAST,
    F_OUTWRTE, F_INWRTE, F_WINTMS, F_RVRTTMS, F_RMPTMS, F_CHAGRISET,
    F_WCHAMAX_SF, F_WCHADISCHAGRA_SF, F_VACHAMAX_SF, F_MINRSVPCT_SF,
    F_CHASTATE_SF, F_STORAVAL_SF, F_INBATV_SF, F_INOUTWRTE_SF,
};
_Static_assert(F_INOUTWRTE_SF == M124_LEN - 1, "model 124 layout");

/* Modbus function codes. */
#define FC_READ_HOLDING    0x03
#define FC_WRITE_MULTIPLE  0x10
#define MBT_MAX_READ       125
#define MBT_MAX_WRITE      123
#define MBT_MAX_ADU        260

/* Internal result codes. Negative = failure; the connection struct carries
 * the detail for the user-facing message. */
enum {
    FS_OK          =  0,
    FS_E_IO        = -1,   /* send/recv failed or timed out             */
    FS_E_FRAME     = -2,   /* answer is not a valid Modbus-TCP frame    */
    FS_E_EXC       = -3,   /* Modbus exception                          */
    FS_E_FOREIGN   = -4,   /* frame for another transaction/unit        */
    FS_E_VERIFY    = -5,   /* written, but the read-back differs        */
    FS_E_NOTFOUND  = -6,   /* no SunSpec map / no model 124             */
    FS_E_NODEV     = -7,   /* no Fronius inverter configured            */
    FS_E_CONNECT   = -8,   /* TCP connect failed                        */
    FS_E_SF        = -9,   /* scale factor missing / value unencodable  */
    FS_E_BASE      = -10,  /* a base value to change, but none is known */
};

/* ------------------------------ I/O hooks -------------------------------
 * The socket layer is the only part that cannot run on the host. It sits
 * behind four function pointers so the host suite can put a simulated
 * inverter (a register map) there and run everything above it unchanged:
 * framing, the model walk, the write/verify sequences and their timing. */
typedef struct {
    int  (*open)(const char *ip, uint16_t port, int timeout_ms); /* handle >= 0 */
    int  (*send)(int h, const uint8_t *buf, int len);            /* 0 = all sent */
    int  (*recv)(int h, uint8_t *buf, int len);                  /* 0 = exactly len */
    void (*close)(int h, bool broken);                           /* broken: a transaction failed */
} fs_io_t;

#ifndef FRONIUS_STORAGE_HOST_TEST
#include "lwip/sockets.h"

/* No connection of our own: the batch runs on the one the Modbus-TCP poll
 * keeps to this address anyway (modbus_tcp_borrow). The datalogger sees one
 * client, and a batch no longer needs a free socket on our side -- the
 * second connection per batch used to fail exactly when the pool was
 * tight. The poll of that address waits while we hold it. */
static int sock_open(const char *ip, uint16_t port, int timeout_ms)
{
    (void)timeout_ms;
    return modbus_tcp_borrow(ip, port, FS_BORROW_WAIT_MS);
}

static int sock_send(int h, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int r = send(h, buf + sent, len - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

static int sock_recv(int h, uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = recv(h, buf + got, len - got, 0);
        if (r <= 0) return -1;            /* timeout, reset or orderly close */
        got += r;
    }
    return 0;
}

static void sock_close(int h, bool broken) { modbus_tcp_give_back(h, broken); }

static const fs_io_t s_sock_io = { sock_open, sock_send, sock_recv, sock_close };
static const fs_io_t *s_io = &s_sock_io;
#else
static const fs_io_t *s_io;               /* the host suite installs its fake */
#endif

/* --------------------------- pure: framing ------------------------------ */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p)   { return (uint16_t)((p[0] << 8) | p[1]); }

/* FC03 request ADU. Always 12 bytes. */
static int mbt_build_read(uint8_t *b, uint16_t tid, uint8_t unit, uint16_t reg, uint16_t n)
{
    put16(b, tid); put16(b + 2, 0); put16(b + 4, 6);
    b[6] = unit; b[7] = FC_READ_HOLDING;
    put16(b + 8, reg); put16(b + 10, n);
    return 12;
}

/* FC16 request ADU. Returns its length, -1 if n does not fit. */
static int mbt_build_write(uint8_t *b, int max, uint16_t tid, uint8_t unit,
                           uint16_t reg, uint16_t n, const uint16_t *v)
{
    if (n == 0 || n > MBT_MAX_WRITE || max < 13 + 2 * n) return -1;
    put16(b, tid); put16(b + 2, 0); put16(b + 4, (uint16_t)(7 + 2 * n));
    b[6] = unit; b[7] = FC_WRITE_MULTIPLE;
    put16(b + 8, reg); put16(b + 10, n); b[12] = (uint8_t)(2 * n);
    for (int i = 0; i < n; i++) put16(b + 13 + 2 * i, v[i]);
    return 13 + 2 * n;
}

/* Checks shared by both answers: MBAP sanity, that the frame is the answer to
 * THIS request, and the exception path. Returns FS_OK when the function code
 * is the expected one and the PDU may be read. */
static int mbt_check(const uint8_t *a, int len, uint16_t tid, uint8_t unit,
                     uint8_t fc, uint8_t *exc)
{
    if (len < 8) return FS_E_FRAME;
    if (get16(a + 2) != 0) return FS_E_FRAME;               /* protocol id */
    if (get16(a + 4) != len - 6) return FS_E_FRAME;         /* MBAP length */
    if (get16(a) != tid || a[6] != unit) return FS_E_FOREIGN;
    if (a[7] == (fc | 0x80)) {
        if (len != 9) return FS_E_FRAME;
        if (exc) *exc = a[8];
        return FS_E_EXC;
    }
    if (a[7] != fc) return FS_E_FRAME;
    return FS_OK;
}

static int mbt_parse_read(const uint8_t *a, int len, uint16_t tid, uint8_t unit,
                          uint16_t n, uint16_t *out, uint8_t *exc)
{
    int rc = mbt_check(a, len, tid, unit, FC_READ_HOLDING, exc);
    if (rc) return rc;
    if (len != 9 + 2 * n || a[8] != 2 * n) return FS_E_FRAME;
    for (int i = 0; i < n; i++) out[i] = get16(a + 9 + 2 * i);
    return FS_OK;
}

/* The FC16 answer echoes start and count. A different echo means the device
 * wrote something other than what we asked for -- treat it as a broken frame,
 * the read-back that follows every write will say what actually happened. */
static int mbt_parse_write(const uint8_t *a, int len, uint16_t tid, uint8_t unit,
                           uint16_t reg, uint16_t n, uint8_t *exc)
{
    int rc = mbt_check(a, len, tid, unit, FC_WRITE_MULTIPLE, exc);
    if (rc) return rc;
    if (len != 12 || get16(a + 8) != reg || get16(a + 10) != n) return FS_E_FRAME;
    return FS_OK;
}

/* --------------------------- pure: scaling ------------------------------ */

/* raw * 10^sf. NAN for "not implemented" markers or an absurd exponent: a
 * missing value must stay recognisably missing, never turn into 0. */
static float ss_scale(uint16_t raw, uint16_t sf_raw, bool is_signed)
{
    int16_t sf = (int16_t)sf_raw;
    if (sf_raw == SS_NI_I16 || sf < -10 || sf > 10) return NAN;
    float v;
    if (is_signed) {
        if (raw == SS_NI_I16) return NAN;
        v = (float)(int16_t)raw;
    } else {
        if (raw == SS_NI_U16) return NAN;
        v = (float)raw;
    }
    return v * powf(10.0f, (float)sf);
}

/* Inverse of ss_scale(): the register value for `val`. false when the scale
 * factor is unusable or the result does not fit the register type. */
static bool ss_unscale(float val, uint16_t sf_raw, bool is_signed, uint16_t *out)
{
    int16_t sf = (int16_t)sf_raw;
    if (!isfinite(val) || sf_raw == SS_NI_I16 || sf < -10 || sf > 10) return false;
    float r = roundf(val / powf(10.0f, (float)sf));
    if (is_signed) {
        if (r < -32767.0f || r > 32767.0f) return false;    /* 0x8000 = NI */
        *out = (uint16_t)(int16_t)r;
    } else {
        if (r < 0.0f || r > 65534.0f) return false;         /* 0xFFFF = NI */
        *out = (uint16_t)r;
    }
    return true;
}

/* ------------------------------- state ---------------------------------- */

/* What the inverter should be doing. BLOCK comes from the energy manager,
 * CHARGE/DISCHARGE from the operator (and win over the block while they run);
 * NONE = our hands off, the originals back. */
enum { FS_M_NONE = 0, FS_M_BLOCK, FS_M_CHARGE, FS_M_DISCHARGE };

static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static fronius_storage_status_t s_st;            /* under s_mux            */
static bool              s_req_on;               /* under s_mux            */
static uint16_t          s_req_rvrt;             /* under s_mux            */
static uint32_t          s_req_seq;              /* under s_mux            */
/* The operator's forced mode -- under s_mux. Not persisted on purpose: after
 * a restart of the display nobody may be watching, so a forced charge or
 * discharge ends with it (the registers are put back, see `owed`). */
typedef struct {
    uint8_t  mode;                                /* FRONIUS_FORCE_*        */
    uint16_t w;
    int64_t  until;                               /* unix                   */
    float    soc;                                 /* stop at this SoC       */
} fs_man_t;
static fs_man_t          s_man;
static char              s_man_end[48];          /* under s_mux            */
static uint8_t           s_req_cap;              /* under s_mux: charge cap %, 0 = off */
static bool              s_req_lock;             /* under s_mux: the operator's discharge lock */

/* Serialises the network work of tick and set_reserve. Everything below is
 * only touched with it held. */
static SemaphoreHandle_t s_lock;

static struct {
    char     ip[32];
    uint16_t port;
    uint8_t  unit;
    uint16_t addr;       /* model 124 start (header), 0 = rediscover */
} s_dev;

/* s_owned: we have written to the storage registers and not yet put them
 * back. Set BEFORE the first write -- a block that half-failed still has to
 * be undone -- and cleared only by a verified restore. While it is set the
 * device is never switched (the restore must reach the same inverter).
 * s_in_touched: the same for InWRte, which only the forced modes write. */
static bool     s_owned;
static bool     s_orig_known;
static uint16_t s_orig_ctl, s_orig_out, s_orig_rvrt;
static bool     s_in_known, s_in_touched;
static uint16_t s_orig_in;
static bool     s_no_revert;         /* inverter ignores RvrtTms (reads 0xFFFF) */
static bool     s_loaded;            /* persisted state read                 */

/* The block state survives a restart of the display. Without it a block
 * that was in force when the display went down would stay in the inverter
 * forever: this Fronius has no working revert timer, and the RAM copy of the
 * originals is gone -- the release would then write defaults instead of the
 * operator's own limits (2026-10-21: StorCtl_Mod 3, OutWRte 26.04 %).
 *
 * The InWRte fields came later and are APPENDED with the version left at 1:
 * nvs_store reads a longer record by its prefix, so a rolled-back firmware
 * still finds and releases a block. It cannot put InWRte back, though --
 * hence fronius_storage_rollback_safe(). */
typedef struct {
    uint16_t ver;
    uint8_t  owned, orig_known, no_revert, unit;
    uint16_t ctl, out, rvrt, port;
    char     ip[32];
    uint16_t in;
    uint8_t  in_known, in_touched;
} fs_rec_t;
#define FS_REC_VER 1

static void fs_persist(void)
{
    fs_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = FS_REC_VER;
    r.owned = s_owned; r.orig_known = s_orig_known; r.no_revert = s_no_revert;
    r.ctl = s_orig_ctl; r.out = s_orig_out; r.rvrt = s_orig_rvrt;
    r.in = s_orig_in; r.in_known = s_in_known; r.in_touched = s_in_touched;
    r.port = s_dev.port; r.unit = s_dev.unit;
    memcpy(r.ip, s_dev.ip, sizeof(r.ip));
    if (nvs_store_set_fs_state(&r, sizeof(r)) != ESP_OK)
        ESP_LOGE(TAG, "block state NOT saved -- a restart now would forget the block");
}

static void fs_load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    fs_rec_t r;
    memset(&r, 0, sizeof(r));
    if (nvs_store_get_fs_state(&r, sizeof(r)) != ESP_OK || r.ver != FS_REC_VER) return;
    s_no_revert = r.no_revert;
    if (!r.owned) return;
    s_owned = true;
    s_orig_known = r.orig_known;
    s_orig_ctl = r.ctl; s_orig_out = r.out; s_orig_rvrt = r.rvrt;
    s_orig_in = r.in; s_in_known = r.in_known; s_in_touched = r.in_touched;
    memcpy(s_dev.ip, r.ip, sizeof(s_dev.ip));
    s_dev.ip[sizeof(s_dev.ip) - 1] = '\0';
    s_dev.port = r.port; s_dev.unit = r.unit; s_dev.addr = 0;
    ESP_LOGW(TAG, "restart with our settings still in %s -- originals %s (StorCtl_Mod=%u OutWRte=%u%s)",
             s_dev.ip, s_orig_known ? "known" : "unknown", s_orig_ctl, s_orig_out,
             s_in_touched ? ", InWRte changed too" : "");
}

/* What was written last and verified at the last read. */
typedef struct { uint16_t ctl, out, in; bool set_in; } fs_tgt_t;
static bool     s_active;            /* s_applied is in force               */
static uint8_t  s_applied;           /* FS_M_*                              */
static uint16_t s_applied_w;
static bool     s_applied_cap;       /* the charge cap is part of s_tgt      */
static bool     s_cap_want;          /* SoC at the cap (with hysteresis)     */
static fs_tgt_t s_tgt;
static uint16_t s_applied_rvrt;
static int64_t  s_last_read, s_last_refresh;
static uint32_t s_seen_seq;
static uint16_t s_tid;
static uint32_t s_writes;            /* register writes since boot         */

/* ----------------------------- transactions ----------------------------- */

typedef struct {
    int      h;
    bool     bad;          /* the stream may be out of step: do not reuse it */
    uint8_t  unit;
    const char *ip;
    uint16_t port;
    /* detail of the last failure, for fs_errmsg() */
    uint8_t  exc;
    uint16_t reg;
    uint16_t want, got;
} fs_conn_t;

/* One complete answer ADU: MBAP first, its length field says how much
 * follows. Framing follows the length field, never an assumed size, so a
 * frame that is not ours can be consumed completely and dropped. */
static int fs_recv_adu(fs_conn_t *c, uint8_t *a, int max)
{
    if (s_io->recv(c->h, a, 7) != 0) return FS_E_IO;
    int plen = get16(a + 4);                /* unit + PDU */
    if (get16(a + 2) != 0 || plen < 2 || 6 + plen > max) return FS_E_FRAME;
    if (s_io->recv(c->h, a + 7, plen - 1) != 0) return FS_E_IO;
    return 6 + plen;
}

/* A failure that can leave an answer in flight or half read marks the
 * connection: it goes back to the poll as broken and is rebuilt. */
static int fs_mark(fs_conn_t *c, int rc)
{
    if (rc == FS_E_IO || rc == FS_E_FRAME) c->bad = true;
    return rc;
}

static int fs_read(fs_conn_t *c, uint16_t reg, uint16_t n, uint16_t *out)
{
    uint8_t q[12], a[MBT_MAX_ADU];
    if (n == 0 || n > MBT_MAX_READ) return FS_E_FRAME;
    uint16_t tid = ++s_tid;
    c->reg = reg;
    int ql = mbt_build_read(q, tid, c->unit, reg, n);
    if (s_io->send(c->h, q, ql) != 0) return fs_mark(c, FS_E_IO);
    for (int i = 0; i < FS_RESYNC; i++) {
        int len = fs_recv_adu(c, a, sizeof(a));
        if (len < 0) return fs_mark(c, len);
        int rc = mbt_parse_read(a, len, tid, c->unit, n, out, &c->exc);
        if (rc != FS_E_FOREIGN) return fs_mark(c, rc);
    }
    return fs_mark(c, FS_E_FRAME);
}

static int fs_write(fs_conn_t *c, uint16_t reg, uint16_t val)
{
    uint8_t q[16], a[MBT_MAX_ADU];
    uint16_t tid = ++s_tid;
    c->reg = reg;
    s_writes++;
    int ql = mbt_build_write(q, sizeof(q), tid, c->unit, reg, 1, &val);
    if (ql < 0) return FS_E_FRAME;
    if (s_io->send(c->h, q, ql) != 0) return fs_mark(c, FS_E_IO);
    for (int i = 0; i < FS_RESYNC; i++) {
        int len = fs_recv_adu(c, a, sizeof(a));
        if (len < 0) return fs_mark(c, len);
        int rc = mbt_parse_write(a, len, tid, c->unit, reg, 1, &c->exc);
        if (rc != FS_E_FOREIGN) return fs_mark(c, rc);
    }
    return fs_mark(c, FS_E_FRAME);
}

static void fs_close(fs_conn_t *c)
{
    if (c->h >= 0) s_io->close(c->h, c->bad);
    c->h = -1;
}

/* Find the SunSpec map and walk its model chain to model 124. Returns the
 * model's start register (its header) in *addr. The address is NOT hard-coded:
 * it moves with the inverter's model list (an added MPPT, another firmware). */
static int fs_find_124(fs_conn_t *c, uint16_t *addr)
{
    static const uint16_t bases[] = { 40000, 0, 50000 };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        uint16_t m[2];
        int rc = fs_read(c, bases[b], 2, m);
        if (rc == FS_E_IO) return rc;              /* dead: no point trying on */
        if (rc != FS_OK || m[0] != SUNS_HI || m[1] != SUNS_LO) continue;
        uint32_t off = (uint32_t)bases[b] + 2;
        for (int k = 0; k < FS_WALK_MAX && off + 2 <= 0xFFFF; k++) {
            uint16_t h[2];
            rc = fs_read(c, (uint16_t)off, 2, h);
            if (rc != FS_OK) return rc;
            if (h[0] == 0xFFFF || h[1] == 0) break;          /* end marker */
            if (h[0] == M124_ID) {
                if (h[1] < M124_LEN) return FS_E_NOTFOUND;
                *addr = (uint16_t)off;
                return FS_OK;
            }
            off += 2u + h[1];
        }
        return FS_E_NOTFOUND;
    }
    return FS_E_NOTFOUND;
}

/* The full model (header + payload) in one read. The header is checked every
 * time: if model 124 is no longer where we found it, the cached address is
 * stale and the next batch has to walk the chain again. */
static int fs_read_model(fs_conn_t *c, uint16_t m[M124_REGS])
{
    int rc = fs_read(c, s_dev.addr, M124_REGS, m);
    if (rc != FS_OK) return rc;
    if (m[0] != M124_ID || m[1] < M124_LEN) return FS_E_NOTFOUND;
    return FS_OK;
}

static uint16_t reg_of(int field) { return (uint16_t)(s_dev.addr + 2 + field); }

/* German message for the status page. */
static void fs_errmsg(int rc, const fs_conn_t *c, char *buf, size_t n)
{
    const char *ip = (c && c->ip) ? c->ip : "?";
    switch (rc) {
    case FS_E_NODEV:    snprintf(buf, n, "Kein Fronius-Wechselrichter (Modbus TCP) aktiv"); break;
    case FS_E_CONNECT:  snprintf(buf, n, "Keine Verbindung zu %s:%u", ip, c ? c->port : 0); break;
    case FS_E_IO:       snprintf(buf, n, "Keine Antwort von %s", ip); break;
    case FS_E_FRAME:    snprintf(buf, n, "Ungültige Modbus-Antwort von %s", ip); break;
    case FS_E_EXC:      snprintf(buf, n, "Modbus-Ausnahme %u bei Register %u",
                                 c ? c->exc : 0, c ? c->reg : 0); break;
    case FS_E_NOTFOUND: snprintf(buf, n, "SunSpec-Speichermodell 124 nicht gefunden (%s)", ip); break;
    case FS_E_VERIFY:   snprintf(buf, n, "Nicht übernommen: Register %u = %u statt %u",
                                 c ? c->reg : 0, c ? c->got : 0, c ? c->want : 0); break;
    case FS_E_SF:       snprintf(buf, n, "Skalenfaktor fehlt, Wert nicht darstellbar"); break;
    case FS_E_BASE:     snprintf(buf, n, "Ursprungswerte unbekannt -- erst die Sperre aufheben"); break;
    default:            snprintf(buf, n, "Fehler %d", rc); break;
    }
}

static bool dev_is_candidate(const mb_dev_cfg_t *d)
{
    return d->enabled && d->mfr == MB_MFR_FRONIUS && d->role == MB_ROLE_INVERTER && d->ip[0];
}

static int fs_open(fs_conn_t *c, const char *ip, uint16_t port, uint8_t unit)
{
    memset(c, 0, sizeof(*c));
    c->ip = ip; c->port = port; c->unit = unit;
    c->h = s_io->open(ip, port, FS_TIMEOUT_MS);
    return c->h < 0 ? FS_E_CONNECT : FS_OK;
}

/* Open a connection to the storage inverter, discovering it (and model 124)
 * when nothing valid is cached. On FS_OK the connection is open and s_dev.addr
 * is set; on failure it is closed. The connection `c` refers to s_dev.ip or to
 * `cand` for its message, so both outlive the call. */
static mb_dev_cfg_t s_cand[MB_MAX_DEVICES];
_Static_assert(sizeof(((mb_dev_cfg_t *)0)->ip) == sizeof(s_dev.ip), "ip buffer sizes");

static int fs_session(fs_conn_t *c)
{
    memset(c, 0, sizeof(*c));
    c->h = -1;
    int n = modbus_tcp_get_devices(s_cand, MB_MAX_DEVICES);
    if (n < 0) n = 0;
    if (n > MB_MAX_DEVICES) n = MB_MAX_DEVICES;

    /* Forget a cached device that was removed or changed in the settings --
     * unless we still owe it a restore. */
    if (s_dev.ip[0] && !s_owned) {
        bool still = false;
        for (int i = 0; i < n; i++)
            if (dev_is_candidate(&s_cand[i]) && !strcmp(s_cand[i].ip, s_dev.ip) &&
                s_cand[i].port == s_dev.port && s_cand[i].slave == s_dev.unit)
                still = true;
        if (!still) memset(&s_dev, 0, sizeof(s_dev));
    }

    if (s_dev.ip[0] && s_dev.addr) {
        int rc = fs_open(c, s_dev.ip, s_dev.port, s_dev.unit);
        if (rc != FS_OK) fs_close(c);
        return rc;
    }

    if (s_owned && s_dev.ip[0]) {
        /* Rediscover on the inverter we changed, and only there. */
        int rc = fs_open(c, s_dev.ip, s_dev.port, s_dev.unit);
        if (rc == FS_OK) {
            uint16_t addr = 0;
            rc = fs_find_124(c, &addr);
            if (rc == FS_OK) { s_dev.addr = addr; return FS_OK; }
        }
        fs_close(c);
        return rc;
    }

    int rc = FS_E_NODEV;
    for (int i = 0; i < n; i++) {
        const mb_dev_cfg_t *d = &s_cand[i];
        if (!dev_is_candidate(d)) continue;
        uint16_t port = d->port ? d->port : 502;
        rc = fs_open(c, d->ip, port, d->slave);
        if (rc == FS_OK) {
            uint16_t addr = 0;
            rc = fs_find_124(c, &addr);
            if (rc == FS_OK) {
                /* memcpy, not snprintf: GCC cannot see that d->ip is
                 * terminated and fails -Wformat-truncation under -Werror. */
                memcpy(s_dev.ip, d->ip, sizeof(s_dev.ip));
                s_dev.ip[sizeof(s_dev.ip) - 1] = 0;
                s_dev.port = port; s_dev.unit = d->slave; s_dev.addr = addr;
                c->ip = s_dev.ip;
                ESP_LOGI(TAG, "%s:%u id%u: model 124 at %u", s_dev.ip, port, d->slave, addr);
                return FS_OK;
            }
        }
        ESP_LOGW(TAG, "%s: no storage model (rc=%d)", d->ip, rc);
        fs_close(c);
    }
    return rc;
}

/* Publish a model read into the status. */
static void fs_publish(const uint16_t m[M124_REGS], int64_t now)
{
    const uint16_t *p = m + 2;
    portENTER_CRITICAL(&s_mux);
    s_st.ok          = true;
    snprintf(s_st.ip, sizeof(s_st.ip), "%s", s_dev.ip);
    s_st.addr        = s_dev.addr;
    s_st.soc_pct     = ss_scale(p[F_CHASTATE],  p[F_CHASTATE_SF],  false);
    s_st.reserve_pct = ss_scale(p[F_MINRSVPCT], p[F_MINRSVPCT_SF], false);
    s_st.wchamax_w   = ss_scale(p[F_WCHAMAX],   p[F_WCHAMAX_SF],   false);
    s_st.out_rte_pct = ss_scale(p[F_OUTWRTE],   p[F_INOUTWRTE_SF], true);
    s_st.in_rte_pct  = ss_scale(p[F_INWRTE],    p[F_INOUTWRTE_SF], true);
    s_st.ctl_mod     = p[F_STORCTL_MOD];
    s_st.rvrt_s      = p[F_RVRTTMS];
    s_st.cha_st      = p[F_CHAST];
    s_st.grid_charge = p[F_CHAGRISET];
    if (now) s_st.read_at = now;
    portEXIT_CRITICAL(&s_mux);
}

static void fs_set_err(int rc, const fs_conn_t *c, bool read_ok)
{
    char e[80] = "";
    if (rc != FS_OK) fs_errmsg(rc, c, e, sizeof(e));
    portENTER_CRITICAL(&s_mux);
    if (!read_ok) { s_st.ok = false; s_st.addr = s_dev.addr; }
    s_st.blocked   = s_active && s_applied == FS_M_BLOCK;
    s_st.forcing   = s_active && (s_applied == FS_M_CHARGE || s_applied == FS_M_DISCHARGE);
    s_st.no_revert = s_no_revert;
    s_st.owed      = s_owned;
    s_st.cap_on     = s_active && s_applied_cap;
    s_st.base_known = s_owned && s_orig_known;
    s_st.base_ctl   = s_orig_ctl;
    s_st.base_out   = s_orig_out;
    s_st.base_in    = s_in_known ? s_orig_in : 0xFFFF;
    snprintf(s_st.err, sizeof(s_st.err), "%s", e);
    portEXIT_CRITICAL(&s_mux);
}

/* Compare a fresh read of the model with the values just written. A device
 * that acknowledges a write and keeps its old value must not count as done. */
static int fs_expect(fs_conn_t *c, const uint16_t *p, int field, uint16_t want)
{
    if (p[field] == want) return FS_OK;
    c->reg = reg_of(field); c->want = want; c->got = p[field];
    ESP_LOGE(TAG, "reg%u = %u: inverter reports %u -- NOT applied", c->reg, want, p[field]);
    return FS_E_VERIFY;
}

/* RvrtTms read-back. Whether the Fronius shows the configured value or the
 * time remaining is not documented; a countdown read a moment after the write
 * would differ by a second or two. Accept a few seconds below the written
 * value -- an ignored write shows the old value (65535 or 0) and still fails. */
#define RVRT_SLACK_S  10
static int fs_expect_rvrt(fs_conn_t *c, const uint16_t *p, uint16_t want)
{
    uint16_t got = p[F_RVRTTMS];
    if (got <= want && got + RVRT_SLACK_S >= want) return FS_OK;
    if (got == 0xFFFF) {
        /* SunSpec "not implemented": the write went in and nothing changed.
         * Seen on the Symo Hybrid 5.0-3-S, fw 0.3.30.0. Not an error of the
         * block -- a property of the device, reported as such. */
        if (!s_no_revert) {
            ESP_LOGW(TAG, "Fronius ignores InOutWRte_RvrtTms -- the block will NOT lift itself");
            s_no_revert = true;
            fs_persist();
        }
        return FS_OK;
    }
    c->reg = reg_of(F_RVRTTMS); c->want = want; c->got = got;
    ESP_LOGE(TAG, "reg%u = %u: inverter reports %u -- NOT applied", c->reg, want, got);
    return FS_E_VERIFY;
}

static bool fs_rate(float pct, const uint16_t *p, uint16_t *out)
{
    return ss_unscale(pct, p[F_INOUTWRTE_SF], true, out);
}

/* The registers for `mode` (w = power of the forced ones).
 *
 * The battery power has to stay inside [-InWRte, +OutWRte] of WChaMax
 * (discharge positive) while the StorCtl_Mod bits are on. A block closes the
 * upper end (OutWRte 0 %). A forced mode sets both ends to the same power with
 * opposite signs, which leaves exactly that power: charge = InWRte +P,
 * OutWRte -P; discharge = OutWRte +P, InWRte -P. Source: community write-ups
 * (libe.net "BYD Modbus", loxforum "Fronius Hybrid with Modbus TCP"), not a
 * Fronius document -- the read-back after every write is the check.
 *
 * NONE puts back what was there; with unknown originals it lifts our limit
 * (discharge bit off, rates to 100 % = no limit). */
static int fs_target(int mode, bool cap, uint16_t w, const uint16_t *p, fs_tgt_t *t)
{
    uint16_t full, zero;
    if (!fs_rate(100.0f, p, &full) || !fs_rate(0.0f, p, &zero)) return FS_E_SF;
    t->set_in = s_in_touched;
    t->in     = (s_orig_known && s_in_known) ? s_orig_in : full;
    switch (mode) {
    case FS_M_NONE:
        if (s_orig_known) { t->ctl = s_orig_ctl; t->out = s_orig_out; }
        else { t->ctl = (uint16_t)(p[F_STORCTL_MOD] & ~CTL_DISCHARGE_LIM); t->out = full; }
        break;
    case FS_M_BLOCK:
        /* bit0 (charge limit) as the operator had it */
        t->ctl = (uint16_t)((s_orig_known ? s_orig_ctl : p[F_STORCTL_MOD]) | CTL_DISCHARGE_LIM);
        t->out = zero;
        break;
    default: {
        float wmax = ss_scale(p[F_WCHAMAX], p[F_WCHAMAX_SF], false);
        if (!isfinite(wmax) || wmax < 1.0f) return FS_E_SF;
        float pct = 100.0f * ((float)w < wmax ? (float)w : wmax) / wmax;
        uint16_t pos, neg;
        if (!fs_rate(pct, p, &pos) || !fs_rate(-pct, p, &neg)) return FS_E_SF;
        t->ctl    = CTL_CHARGE_LIM | CTL_DISCHARGE_LIM;
        t->set_in = true;
        if (mode == FS_M_CHARGE) { t->in = pos; t->out = neg; }
        else                     { t->out = pos; t->in = neg; }
        return FS_OK;            /* forced: the operator's own SoC target rules, not the cap */
    }
    }
    /* The upper charge limit (the Fronius has no register for a maximum SoC):
     * charging held at 0 %, discharging as above. */
    if (cap) {
        t->set_in = true;
        t->in     = zero;
        t->ctl   |= CTL_CHARGE_LIM;
    }
    return FS_OK;
}

static bool fs_intact(const uint16_t *p)
{
    return p[F_OUTWRTE] == s_tgt.out && p[F_STORCTL_MOD] == s_tgt.ctl &&
           (!s_tgt.set_in || p[F_INWRTE] == s_tgt.in);
}

static const char *mode_name(int mode)
{
    switch (mode) {
    case FS_M_BLOCK:     return "discharge block";
    case FS_M_CHARGE:    return "forced charge";
    case FS_M_DISCHARGE: return "forced discharge";
    default:             return "release";
    }
}

/* Bring the inverter to `mode`. Order matters for what a half-done sequence
 * leaves behind:
 *  - the revert timer first (so whatever follows is time-limited), and on a
 *    release last (so the timer we set still runs if the sequence breaks off);
 *  - of the two rates the one that opens up first, the one that closes
 *    second: the allowed window never goes empty in between;
 *  - the enable bits after the values, so a limit is never active with a
 *    stale value. */
static int fs_apply(fs_conn_t *c, uint16_t m[M124_REGS], int mode, bool cap, uint16_t w, uint16_t rvrt)
{
    const uint16_t *p = m + 2;
    fs_tgt_t t;
    int rc = fs_target(mode, cap, w, p, &t);  /* encodable at all? before anything is taken */
    if (rc != FS_OK) return rc;
    bool ours = mode != FS_M_NONE || cap;     /* a setting of ours, not the release */

    if (ours && !s_owned) {
        /* Capture the originals ONCE. A model that already shows "discharge
         * limited to 0 %" or a negative rate is most likely our own setting
         * surviving a reboot of the display (the RAM copy is gone); taking
         * that as the original would make the release a no-op. */
        uint16_t zero = 0;
        fs_rate(0.0f, p, &zero);
        bool leftover = (p[F_OUTWRTE] == zero && (p[F_STORCTL_MOD] & CTL_DISCHARGE_LIM)) ||
                        (int16_t)p[F_OUTWRTE] < 0 || (int16_t)p[F_INWRTE] < 0;
        if (leftover) {
            s_orig_known = false;
            s_in_known   = false;
            ESP_LOGW(TAG, "storage already limited (OutWRte=%u InWRte=%u) -- originals unknown",
                     p[F_OUTWRTE], p[F_INWRTE]);
        } else {
            s_orig_known = true;
            s_orig_ctl  = p[F_STORCTL_MOD];
            s_orig_out  = p[F_OUTWRTE];
            s_orig_rvrt = p[F_RVRTTMS];
            s_orig_in   = p[F_INWRTE];
            s_in_known  = true;
            ESP_LOGI(TAG, "originals: StorCtl_Mod=%u OutWRte=%u InWRte=%u RvrtTms=%u",
                     s_orig_ctl, s_orig_out, s_orig_in, s_orig_rvrt);
        }
        s_owned = true;
        fs_persist();                   /* before the first write, see fs_rec_t */
    }
    if ((mode == FS_M_CHARGE || mode == FS_M_DISCHARGE || cap) && !s_in_touched) {
        if (!s_in_known && s_orig_known) {
            /* owned since a firmware that never wrote InWRte: still the original */
            s_orig_in  = p[F_INWRTE];
            s_in_known = true;
        }
        s_in_touched = true;
        fs_persist();
    }
    if ((rc = fs_target(mode, cap, w, p, &t)) != FS_OK) return rc;

    bool rv_first = ours && !s_no_revert;
    bool rv_last  = !ours && s_orig_known && !s_no_revert;
    int  din  = t.set_in ? (int)(int16_t)t.in - (int)(int16_t)p[F_INWRTE] : 0;
    int  dout = (int)(int16_t)t.out - (int)(int16_t)p[F_OUTWRTE];
    bool in_first = t.set_in && din >= 0 && dout < 0;

    int      f1 = in_first ? F_INWRTE : F_OUTWRTE;
    uint16_t v1 = in_first ? t.in : t.out;
    if (rv_first && (rc = fs_write(c, reg_of(F_RVRTTMS), rvrt)) != FS_OK) return rc;
    if ((rc = fs_write(c, reg_of(f1), v1)) != FS_OK) return rc;
    if (t.set_in) {
        /* Both rates change: the opening one must really be in before the
         * closing one goes -- an ignored first write would otherwise leave
         * a window with nothing in it. */
        if ((rc = fs_read_model(c, m)) != FS_OK) return rc;
        if ((rc = fs_expect(c, p, f1, v1)) != FS_OK) return rc;
        int f2 = in_first ? F_OUTWRTE : F_INWRTE;
        if ((rc = fs_write(c, reg_of(f2), in_first ? t.out : t.in)) != FS_OK) return rc;
    }
    if ((rc = fs_write(c, reg_of(F_STORCTL_MOD), t.ctl)) != FS_OK) return rc;
    if (rv_last && (rc = fs_write(c, reg_of(F_RVRTTMS), s_orig_rvrt)) != FS_OK) return rc;

    if ((rc = fs_read_model(c, m)) != FS_OK) return rc;
    if (rv_first && (rc = fs_expect_rvrt(c, p, rvrt)) != FS_OK) return rc;
    if ((rc = fs_expect(c, p, F_OUTWRTE, t.out)) != FS_OK) return rc;
    if (t.set_in && (rc = fs_expect(c, p, F_INWRTE, t.in)) != FS_OK) return rc;
    if ((rc = fs_expect(c, p, F_STORCTL_MOD, t.ctl)) != FS_OK) return rc;
    if (rv_last && (rc = fs_expect(c, p, F_RVRTTMS, s_orig_rvrt)) != FS_OK) return rc;
    s_tgt = t;
    ESP_LOGW(TAG, "BYD %s%s %u W (OutWRte=%u InWRte=%u StorCtl_Mod=%u%s) verified", mode_name(mode),
             cap ? " + charge cap" : "", w, t.out, p[F_INWRTE], t.ctl, rv_first ? ", revert timer set" : "");
    return FS_OK;
}

/* Keep the inverter's revert timer from running out while the mode is
 * wanted. Only RvrtTms is rewritten; the rest is verified alongside. */
static int fs_refresh(fs_conn_t *c, uint16_t m[M124_REGS], uint16_t rvrt)
{
    const uint16_t *p = m + 2;
    int rc;
    if (!s_no_revert && (rc = fs_write(c, reg_of(F_RVRTTMS), rvrt)) != FS_OK) return rc;
    if ((rc = fs_read_model(c, m)) != FS_OK) return rc;
    if (!s_no_revert && (rc = fs_expect_rvrt(c, p, rvrt)) != FS_OK) return rc;
    if ((rc = fs_expect(c, p, F_OUTWRTE, s_tgt.out)) != FS_OK) return rc;
    if (s_tgt.set_in && (rc = fs_expect(c, p, F_INWRTE, s_tgt.in)) != FS_OK) return rc;
    if ((rc = fs_expect(c, p, F_STORCTL_MOD, s_tgt.ctl)) != FS_OK) return rc;
    ESP_LOGI(TAG, "%s refreshed (RvrtTms=%u)", mode_name(s_applied), rvrt);
    return FS_OK;
}

/* ------------------------------- public --------------------------------- */

bool fronius_storage_owed(void)
{
    fs_load();          /* only ever called from the energy manager task */
    portENTER_CRITICAL(&s_mux);
    bool man = s_man.mode != FRONIUS_FORCE_OFF || s_req_cap != 0 || s_req_lock;
    portEXIT_CRITICAL(&s_mux);
    return s_owned || man;
}

bool fronius_storage_rollback_safe(void)
{
    return !s_in_touched;
}

void fronius_storage_get(fronius_storage_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    out->force       = s_man.mode;
    out->force_w     = s_man.w;
    out->force_until = s_man.mode ? s_man.until : 0;
    out->force_soc   = s_man.soc;
    out->cap_pct     = s_req_cap;
    out->lock_req    = s_req_lock;
    memcpy(out->force_end, s_man_end, sizeof(out->force_end));
    portEXIT_CRITICAL(&s_mux);
}
_Static_assert(sizeof(((fronius_storage_status_t *)0)->force_end) == sizeof(s_man_end), "force_end size");

esp_err_t fronius_storage_block_discharge(bool on, uint16_t revert_s)
{
    if (on) {
        if (revert_s == 0) revert_s = FRONIUS_STORAGE_RVRT_DEF_S;
        if (revert_s < FRONIUS_STORAGE_RVRT_MIN_S) revert_s = FRONIUS_STORAGE_RVRT_MIN_S;
        if (revert_s > FRONIUS_STORAGE_RVRT_MAX_S) revert_s = FRONIUS_STORAGE_RVRT_MAX_S;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_req_on != on || (on && s_req_rvrt != revert_s)) {
        s_req_on = on;
        s_req_rvrt = on ? revert_s : 0;
        s_req_seq++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void fronius_storage_lock_discharge(bool on)
{
    portENTER_CRITICAL(&s_mux);
    if (s_req_lock != on) { s_req_lock = on; s_req_seq++; }
    portEXIT_CRITICAL(&s_mux);
}

void fronius_storage_set_charge_cap(uint8_t max_soc_pct)
{
    if (max_soc_pct >= 100) max_soc_pct = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_req_cap != max_soc_pct) { s_req_cap = max_soc_pct; s_req_seq++; }
    portEXIT_CRITICAL(&s_mux);
}

void fronius_storage_force_stop(const char *why)
{
    portENTER_CRITICAL(&s_mux);
    if (s_man.mode) {
        s_man.mode = FRONIUS_FORCE_OFF;
        snprintf(s_man_end, sizeof(s_man_end), "%s", why ? why : "beendet");
        s_req_seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t fronius_storage_force(fronius_force_t mode, uint16_t w, uint16_t minutes,
                                float soc_pct, int64_t now)
{
    if (mode == FRONIUS_FORCE_OFF) {
        fronius_storage_force_stop("von Hand beendet");
        return ESP_OK;
    }
    if (mode != FRONIUS_FORCE_CHARGE && mode != FRONIUS_FORCE_DISCHARGE) return ESP_ERR_INVALID_ARG;
    if (w < FRONIUS_FORCE_MIN_W || w > FRONIUS_FORCE_MAX_W) return ESP_ERR_INVALID_ARG;
    if (minutes == 0) minutes = FRONIUS_FORCE_DEF_MIN;
    if (minutes > FRONIUS_FORCE_MAX_MIN) return ESP_ERR_INVALID_ARG;
    if (!isfinite(soc_pct) || soc_pct < 0.0f || soc_pct > 100.0f) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_mux);
    s_man.mode  = (uint8_t)mode;
    s_man.w     = w;
    s_man.until = now + (int64_t)minutes * 60;
    s_man.soc   = soc_pct;
    s_man_end[0] = '\0';
    s_req_seq++;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

/* The forced mode `seen` is over. Only if it is still the one requested -- a
 * new request in the meantime must not be cancelled by the old one's end. */
static void man_finish(const fs_man_t *seen, const char *why)
{
    portENTER_CRITICAL(&s_mux);
    bool same = s_man.mode == seen->mode && s_man.until == seen->until;
    if (same) {
        s_man.mode = FRONIUS_FORCE_OFF;
        snprintf(s_man_end, sizeof(s_man_end), "%s", why);
    }
    portEXIT_CRITICAL(&s_mux);
    if (same) ESP_LOGW(TAG, "BYD %s ends: %s", seen->mode == FRONIUS_FORCE_CHARGE ? "forced charge" : "forced discharge", why);
}

void fronius_storage_tick(int64_t now)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    fs_load();

    portENTER_CRITICAL(&s_mux);
    bool     want_on = s_req_on;
    uint16_t rvrt    = s_req_rvrt;
    uint32_t seq     = s_req_seq;
    fs_man_t man     = s_man;
    uint8_t  capp    = s_req_cap;
    bool     lockreq = s_req_lock;
    portEXIT_CRITICAL(&s_mux);
    bool     expired = false;

    /* The ESP's own time limit for a forced mode -- this Fronius has no revert
     * timer that would end it. A clock step backwards must not stretch it. */
    if (man.mode && (now >= man.until || man.until - now > (int64_t)FRONIUS_FORCE_MAX_MIN * 60 + 60)) {
        man_finish(&man, "Zeit abgelaufen");
        man.mode = FRONIUS_FORCE_OFF;
        expired  = true;                          /* act on it in this tick */
    }

    /* Clock steps (NTP sync after boot) count as "due" in either direction. */
    bool read_due    = s_last_read == 0 || now - s_last_read >= FS_READ_EVERY_S ||
                       now < s_last_read;
    bool changed     = seq != s_seen_seq || expired;
    bool refresh_due = s_active && !s_no_revert &&
                       (now - s_last_refresh >= s_applied_rvrt / 3 || now < s_last_refresh);
    if (!read_due && !changed && !refresh_due) return;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(FS_LOCK_WAIT_MS)) != pdTRUE) return;
    s_seen_seq  = seq;
    s_last_read = now;

    fs_conn_t c;
    uint16_t m[M124_REGS];
    int rc = fs_session(&c);
    bool read_ok = false;
    if (rc == FS_OK) {
        rc = fs_read_model(&c, m);
        if (rc == FS_OK) { read_ok = true; fs_publish(m, now); }
    }

    if (rc == FS_OK) {
        const uint16_t *p = m + 2;
        if (man.mode) {
            float soc = ss_scale(p[F_CHASTATE], p[F_CHASTATE_SF], false);
            /* The upper charge limit is general (operator, 2026-10-25: "only
             * charge to 80 %"): a forced charge stops there too. */
            bool  at_cap = capp != 0 && capp < man.soc;
            float top    = at_cap ? (float)capp : (float)man.soc;
            if (isfinite(soc) && ((man.mode == FRONIUS_FORCE_CHARGE && soc >= top) ||
                                  (man.mode == FRONIUS_FORCE_DISCHARGE && soc <= man.soc))) {
                man_finish(&man, man.mode == FRONIUS_FORCE_CHARGE && at_cap ? "Ladegrenze erreicht"
                                                                           : "Ziel-SoC erreicht");
                man.mode = FRONIUS_FORCE_OFF;
            }
        }
        int mode = man.mode == FRONIUS_FORCE_CHARGE    ? FS_M_CHARGE
                 : man.mode == FRONIUS_FORCE_DISCHARGE ? FS_M_DISCHARGE
                 : (want_on || lockreq)                ? FS_M_BLOCK : FS_M_NONE;
        bool     forced = mode == FS_M_CHARGE || mode == FS_M_DISCHARGE;
        uint16_t w  = forced ? man.w : 0;
        /* The manager's block brings its revert time; the operator's lock
         * (bat_lock keeps its duration itself) and the cap take the default. */
        uint16_t rv = forced ? FS_FORCE_RVRT_S : (want_on && rvrt ? rvrt : FRONIUS_STORAGE_RVRT_DEF_S);

        /* The upper charge limit: on at the cap, off again 2 points below it
         * -- without the gap it would toggle with every percent of SoC. */
        float soc = ss_scale(p[F_CHASTATE], p[F_CHASTATE_SF], false);
        if (capp == 0)            s_cap_want = false;
        else if (isfinite(soc))   s_cap_want = s_cap_want ? soc > (float)capp - FS_CAP_HYST : soc >= (float)capp;
        bool cap = s_cap_want && !forced;

        if (mode != FS_M_NONE || cap) {
            /* Intact = what we wrote, AND still self-reverting: a setting
             * whose timer was reset to "never" must not stand unrefreshed. */
            bool intact = fs_intact(p) &&
                          (s_no_revert || (p[F_RVRTTMS] != 0 && p[F_RVRTTMS] <= s_applied_rvrt));
            if (s_active && !intact) {
                /* Revert timer ran out, or the Fronius' own management took
                 * its limits back: the setting is gone, apply it again. */
                ESP_LOGW(TAG, "%s no longer in force (OutWRte=%u InWRte=%u StorCtl_Mod=%u) -- reapplying",
                         mode_name(s_applied), p[F_OUTWRTE], p[F_INWRTE], p[F_STORCTL_MOD]);
                s_active = false;
            }
            if (!s_active || mode != s_applied || cap != s_applied_cap || w != s_applied_w ||
                rv != s_applied_rvrt) {
                s_active = false;
                rc = fs_apply(&c, m, mode, cap, w, rv);
                if (rc == FS_OK) {
                    s_active = true; s_applied = (uint8_t)mode; s_applied_cap = cap; s_applied_w = w;
                    s_applied_rvrt = rv; s_last_refresh = now;
                }
            } else if (refresh_due) {
                rc = fs_refresh(&c, m, rv);
                if (rc == FS_OK) s_last_refresh = now;
                else s_active = false;    /* retried as a full apply next time */
            }
        } else if (s_owned) {
            s_active = false;
            rc = fs_apply(&c, m, FS_M_NONE, false, 0, 0);
            if (rc == FS_OK) {
                s_owned = s_orig_known = s_in_known = s_in_touched = false;
                s_applied = FS_M_NONE; s_applied_cap = false;
                fs_persist();
            }
        }
        if (rc == FS_OK || rc == FS_E_VERIFY || rc == FS_E_EXC) fs_publish(m, 0);
    }

    if (rc != FS_OK) {
        if (rc != FS_E_VERIFY && rc != FS_E_EXC && rc != FS_E_SF) {
            /* Connection or map trouble: walk the chain again next time. */
            s_dev.addr = 0;
            s_active = false;
        }
        ESP_LOGW(TAG, "tick: rc=%d", rc);
    }
    fs_set_err(rc, &c, read_ok);
    fs_close(&c);
    xSemaphoreGive(s_lock);
}

/* One of the operator's own settings, by hand (the sliders of the Fronius
 * card). Settings the module does not drive go to the inverter at once.
 * While a block or forced mode of ours is in force, the registers it drives
 * belong to it: then the value becomes the operator's BASE value -- the one
 * put back when the block ends -- and the live register is left alone (it
 * would otherwise be reapplied by the next tick anyway). */
esp_err_t fronius_storage_set(fronius_reg_t reg, float val, bool *as_base)
{
    if (as_base) *as_base = false;
    if (!isfinite(val)) return ESP_ERR_INVALID_ARG;
    switch (reg) {
    case FRONIUS_REG_RESERVE: case FRONIUS_REG_OUT: case FRONIUS_REG_IN:
        if (val < 0.0f || val > 100.0f) return ESP_ERR_INVALID_ARG;
        break;
    case FRONIUS_REG_CTL:
        if (val < 0.0f || val > 3.0f || val != floorf(val)) return ESP_ERR_INVALID_ARG;
        break;
    case FRONIUS_REG_GRID:
        if (val != 0.0f && val != 1.0f) return ESP_ERR_INVALID_ARG;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(FS_LOCK_WAIT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;

    fs_conn_t c;
    uint16_t m[M124_REGS];
    const uint16_t *p = m + 2;
    uint16_t raw = 0;
    int field = -1;
    bool read_ok = false, base = false;
    int rc = fs_session(&c);
    if (rc == FS_OK) rc = fs_read_model(&c, m);
    if (rc == FS_OK) {
        read_ok = true;
        switch (reg) {
        case FRONIUS_REG_RESERVE:
            field = F_MINRSVPCT;
            if (!ss_unscale(val, p[F_MINRSVPCT_SF], false, &raw)) rc = FS_E_SF;
            break;
        case FRONIUS_REG_OUT:
            field = F_OUTWRTE;
            if (!fs_rate(val, p, &raw)) rc = FS_E_SF;
            base = s_owned;                       /* every mode of ours drives OutWRte */
            break;
        case FRONIUS_REG_IN:
            field = F_INWRTE;
            if (!fs_rate(val, p, &raw)) rc = FS_E_SF;
            base = s_owned && s_in_touched;       /* only the forced modes drive InWRte */
            break;
        case FRONIUS_REG_CTL:
            field = F_STORCTL_MOD; raw = (uint16_t)val;
            base = s_owned;
            break;
        case FRONIUS_REG_GRID:
            field = F_CHAGRISET; raw = (uint16_t)val;
            break;
        }
    }
    if (rc == FS_OK && base && !s_orig_known) {
        /* Our setting is in, but what was there before is not known (it
         * survived a reboot of the display): no base to change. */
        rc = FS_E_BASE;
    } else if (rc == FS_OK && base) {
        if (field == F_OUTWRTE)     s_orig_out = raw;
        if (field == F_INWRTE)      s_orig_in  = raw;
        if (field == F_STORCTL_MOD) s_orig_ctl = raw;
        fs_persist();
        s_active = false;                         /* the next tick reapplies with the new base */
        portENTER_CRITICAL(&s_mux);
        s_req_seq++;                              /* ... at once, not at the next read */
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "base value reg%u = %u (applies after our setting ends)", reg_of(field), raw);
    } else if (rc == FS_OK) {
        rc = fs_write(&c, reg_of(field), raw);
        if (rc == FS_OK) rc = fs_read_model(&c, m);
        if (rc == FS_OK) rc = fs_expect(&c, p, field, raw);
        if (rc == FS_OK) {
            ESP_LOGW(TAG, "reg%u = %u by hand, verified", reg_of(field), raw);
            /* InWRte while a block holds the others: the base follows, so
             * the release does not take the new value back. */
            if (field == F_INWRTE && s_owned && s_in_known) { s_orig_in = raw; fs_persist(); }
        }
    }
    if (read_ok && (rc == FS_OK || rc == FS_E_VERIFY || rc == FS_E_EXC || rc == FS_E_BASE)) fs_publish(m, 0);
    if (rc != FS_OK && rc != FS_E_VERIFY && rc != FS_E_EXC && rc != FS_E_SF && rc != FS_E_BASE)
        s_dev.addr = 0;
    fs_set_err(rc, &c, read_ok);
    fs_close(&c);
    xSemaphoreGive(s_lock);
    if (as_base) *as_base = base && rc == FS_OK;
    return rc == FS_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t fronius_storage_set_reserve(float pct)
{
    return fronius_storage_set(FRONIUS_REG_RESERVE, pct, NULL);
}

#include "modbus_tcp.h"
#include "nvs_store.h"
#include "ui_flow.h"
#include "lvgl_port.h"
#include "deye_ctrl.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "modbus";

#define MB_FC03  0x03
#define MB_FC04  0x04

/* ---- Deye SG04LP3 (FC03) register map (Modbus Deye - MQTT HA project) ---- */
#define DEYE_HOT_START   586
#define DEYE_HOT_COUNT   53
#define DEYE_C2_START    644
#define DEYE_C2_COUNT    40
#define DEYE_BATT_SOC    588
#define DEYE_BATT_POWER  590    /* S16, +discharge/-charge */
#define DEYE_GRID_CT_TOT 619    /* S16 */
#define DEYE_LOAD_TOT    653    /* S16 */
#define DEYE_PV1         672
#define DEYE_PV2         673

/* ---- Eltako DSZ15/DSZ16 (FC04): total active power, signed int32 watts ----
 * Register 52 (0x0034); same address SDM630 uses for float32, but Eltako meters
 * encode it as an integer -- decode with words_to_i32, not words_to_f32. */
#define SDM630_PTOT_ADDR 0x0034

static modbus_tcp_status_t s_st;
static portMUX_TYPE        s_mux    = portMUX_INITIALIZER_UNLOCKED;
static volatile bool       s_reconf = true;

static mb_dev_cfg_t  s_devs[MB_MAX_DEVICES];
static int           s_dev_count;
static mb_dev_live_t s_live[MB_MAX_DEVICES];
static int           s_live_count;

/* Freshness of the grid (netz) value -- the ONLY value that drives the inverter
 * (via the RTU Eastron emulation's zero-export trick). A frozen grid_w fed into
 * that control loop once drove a 15 kW export runaway, so consumers that steer
 * the Deye must use modbus_tcp_grid_w_fresh() and refuse to act when it is
 * stale. Updated only on a fresh successful read of the grid source. */
static uint32_t s_grid_ms;        /* esp_timer ms of last fresh grid update */
static bool     s_grid_valid;     /* grid_w ever read since (re)config       */
static bool     s_have_grid_role; /* any enabled device has role GRID        */

/* Cached SunSpec layout per device. Walking the model list costs one Modbus
 * read per model and was redone for EVERY value on EVERY poll (50-100 reads per
 * Fronius per cycle -> ~30 s cycles with several inverters). The layout is fixed
 * per device, so discover it once (sunspec_discover) and cache the offsets;
 * steady-state polls then issue only the handful of value reads. */
typedef struct {
    bool     done;        /* discovery succeeded (base found)             */
    uint16_t base;        /* SunSpec base address                         */
    uint16_t inv_off;     /* AC power model data offset (0 = none)        */
    uint8_t  inv_float;   /* 0 = int model 101-103, 1 = float 111-113     */
    uint16_t soc_off;     /* storage model 124 data offset (0 = none)     */
    uint16_t mppt_off;    /* MPPT model 160 data offset (0 = none)        */
    uint16_t mppt_len;
    uint16_t meter_off;   /* meter model 201-204 data offset (0 = none)   */
} ss_cache_t;
static ss_cache_t s_ss[MB_MAX_DEVICES];

/* Deye supplied by the RTU master (modbus_rtu.c), if present. Carries a
 * timestamp so a value stops "ghosting" once the master stops refreshing it
 * (e.g. the Deye-read bus is switched off / set to slave) -- without an expiry
 * the last SoC+power stuck on the display forever. Generous (display-only, not
 * a control value) so normal RTU read jitter never flickers the node to "--";
 * still clears within a sensible window once the Deye-read bus truly stops. */
#define MB_RTU_DEYE_MAX_AGE_MS 20000
static volatile bool     s_rtu_deye_valid;
static volatile float    s_rtu_deye_w, s_rtu_deye_soc;
static volatile uint32_t s_rtu_deye_ms;

void modbus_tcp_set_rtu_deye(float w, float soc, bool valid)
{
    s_rtu_deye_w   = w;
    s_rtu_deye_soc = soc;
    s_rtu_deye_valid = valid;
    if (valid) s_rtu_deye_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* Previous ("devs4") record size: mb_dev_cfg_t without the trailing name[24].
 * Its bytes match the current struct's first 42 bytes. */
#define MB_CFG_V4_SIZE 42

/* Load the device list from NVS into a caller-owned buffer (NO shared state):
 * the result is committed into s_devs under the lock by reconfigure_apply(), so
 * the parallel workers always snapshot a consistent list. Returns the count. */
static int load_cfg_into(mb_dev_cfg_t *devs)
{
    size_t len = sizeof(mb_dev_cfg_t) * MB_MAX_DEVICES;
    memset(devs, 0, len);
    int cnt = 0;

    if (nvs_store_get_mb_devices(devs, &len) == ESP_OK) {
        cnt = (int)(len / sizeof(mb_dev_cfg_t));
        if (cnt > MB_MAX_DEVICES) cnt = MB_MAX_DEVICES;
    } else {
        /* One-time migration from the previous layout so the user keeps their
         * devices. Old records are 42 bytes; copy them in, name stays empty. */
        uint8_t old[MB_MAX_DEVICES * MB_CFG_V4_SIZE];
        size_t olen = sizeof(old);
        if (nvs_store_get_mb_devices_legacy(old, &olen) == ESP_OK) {
            int n = (int)(olen / MB_CFG_V4_SIZE);
            if (n > MB_MAX_DEVICES) n = MB_MAX_DEVICES;
            for (int i = 0; i < n; i++) {
                memcpy(&devs[i], old + i * MB_CFG_V4_SIZE, MB_CFG_V4_SIZE);
                devs[i].name[0] = '\0';
            }
            cnt = n;
            if (n > 0) {
                nvs_store_set_mb_devices(devs, (size_t)n * sizeof(mb_dev_cfg_t));
                ESP_LOGW(TAG, "migrated %d device(s) from old NVS layout", n);
            }
        }
    }

    /* Clamp poll interval + timeout. */
    for (int i = 0; i < cnt; i++) {
        if (devs[i].poll_ms == 0) devs[i].poll_ms = MB_DEFAULT_POLL_MS;
        if (devs[i].poll_ms < MB_MIN_POLL_MS) devs[i].poll_ms = MB_MIN_POLL_MS;
        if (devs[i].poll_ms > MB_MAX_POLL_MS) devs[i].poll_ms = MB_MAX_POLL_MS;
        if (devs[i].timeout_ms == 0) devs[i].timeout_ms = MB_DEFAULT_TIMEOUT_MS;
        if (devs[i].timeout_ms < MB_MIN_TIMEOUT_MS) devs[i].timeout_ms = MB_MIN_TIMEOUT_MS;
        if (devs[i].timeout_ms > MB_MAX_TIMEOUT_MS) devs[i].timeout_ms = MB_MAX_TIMEOUT_MS;
    }
    ESP_LOGI(TAG, "loaded %d Modbus device(s)", cnt);
    return cnt;
}

/* Compile-time guard: the current struct appends name[24] to the 42-byte
 * devs4 layout -> 66. */
_Static_assert(sizeof(mb_dev_cfg_t) == MB_CFG_V4_SIZE + 24, "mb_dev_cfg_t layout");

/* --------------------------- socket + framing -------------------------- */

/* connect_ms bounds the TCP handshake (a slow datalogger needs more grace than
 * a single read), io_ms bounds each subsequent read/write. */
static int connect_timeout(const char *ip, uint16_t port, int connect_ms, int io_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -1; }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) { close(s); return -1; }
    if (r < 0) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv = { .tv_sec = connect_ms / 1000, .tv_usec = (connect_ms % 1000) * 1000 };
        if (select(s + 1, NULL, &w, NULL, &tv) <= 0) { close(s); return -1; }
        int err = 0; socklen_t el = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(s); return -1; }
    }
    fcntl(s, F_SETFL, fl);
    struct timeval to = { .tv_sec = io_ms / 1000, .tv_usec = (io_ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    /* close() sends RST (no graceful FIN) -> the socket frees IMMEDIATELY with
     * NO TIME_WAIT. Critical here: a flaky inverter (connect storms / dropped
     * reads) would otherwise pile up TIME_WAIT sockets and exhaust the shared
     * LWIP pool, starving the HTTP servers. Safe for a polling Modbus client. */
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    return s;
}

static int recv_all(int s, uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int mb_read(int s, uint8_t unit, uint8_t fc,
                   uint16_t addr, uint16_t count, uint16_t *out)
{
    uint8_t req[12] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x06, unit, fc,
        (uint8_t)(addr >> 8), (uint8_t)addr,
        (uint8_t)(count >> 8), (uint8_t)count,
    };
    if (send(s, req, sizeof(req), 0) != (int)sizeof(req)) return -1;
    uint8_t h[9];
    if (recv_all(s, h, 9) < 0) return -1;
    if (h[7] != fc) return -2;
    int bc = h[8];
    if (bc != count * 2 || bc > 250) return -3;
    uint8_t d[250];
    if (recv_all(s, d, bc) < 0) return -1;
    for (int i = 0; i < count; i++) out[i] = (uint16_t)((d[i * 2] << 8) | d[i * 2 + 1]);
    return 0;
}

static float words_to_f32(uint16_t hi, uint16_t lo)
{
    uint32_t v = ((uint32_t)hi << 16) | lo;
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* Signed 32-bit integer from two registers, high word first. Eltako DSZ meters
 * encode power/energy as int32 (scale factor 1 => watts), NOT IEEE-754 float. */
static int32_t words_to_i32(uint16_t hi, uint16_t lo)
{
    return (int32_t)(((uint32_t)hi << 16) | lo);
}

/* Apply a SunSpec scale factor safely. Real SFs are tiny (~-10..10); a garbage
 * value (e.g. from a desynced read) must NOT become 10^big = inf and poison the
 * aggregate. Clamp the exponent and force a finite result. */
static float apply_sf(float raw, int16_t sf)
{
    if (sf < -10) sf = -10;
    if (sf >  10) sf =  10;
    float v = raw * powf(10.0f, (float)sf);
    return isfinite(v) ? v : 0.0f;
}

/* Plausibility gate for any power reading (W).
 * A corrupt or partial Modbus read can produce a finite IEEE-754 float that is
 * physically impossible (e.g. 3e11 W from a garbled SunSpec float register).
 * Anything beyond ±500 kW is rejected as a read error. */
#define MB_MAX_PLAUSIBLE_W  100000.0f   /* 100 kW >> 35A/3ph (~24 kW) Hausanschluss */
static bool plausible_w(float v) { return isfinite(v) && fabsf(v) <= MB_MAX_PLAUSIBLE_W; }

/* --------------------------- SunSpec helpers --------------------------- */

/* Find the SunSpec base register ("SunS" marker). 0xFFFF if not found. */
static uint16_t sunspec_base(int s, uint8_t u)
{
    static const uint16_t bases[] = { 40000, 0, 50000 };
    uint16_t m[2];
    for (int b = 0; b < 3; b++)
        if (mb_read(s, u, MB_FC03, bases[b], 2, m) == 0 &&
            m[0] == 0x5375 && m[1] == 0x6E53)
            return bases[b];
    return 0xFFFF;
}

/* Walk the model list, return the DATA offset of model `id` (0 if absent). */
static uint16_t sunspec_model(int s, uint8_t u, uint16_t base, uint16_t id, uint16_t *len)
{
    uint16_t off = base + 2;
    for (int n = 0; n < 32; n++) {
        uint16_t h[2];
        if (mb_read(s, u, MB_FC03, off, 2, h)) return 0;
        if (h[0] == 0xFFFF) return 0;
        if (h[0] == id) { if (len) *len = h[1]; return off + 2; }
        if (h[1] == 0) return 0;
        off += 2 + h[1];
    }
    return 0;
}

/* Discover the SunSpec layout ONCE and cache the model offsets. This is the
 * expensive part (walks the model list); steady-state polls reuse the cache. */
static void sunspec_discover(int s, uint8_t u, ss_cache_t *c)
{
    memset(c, 0, sizeof(*c));
    c->base = sunspec_base(s, u);
    if (c->base == 0xFFFF) return;            /* no SunSpec -> retry next poll */

    uint16_t len;
    for (uint16_t id = 101; id <= 103 && !c->inv_off; id++) {
        uint16_t off = sunspec_model(s, u, c->base, id, &len);
        if (off) { c->inv_off = off; c->inv_float = 0; }   /* int W@12, SF@13 */
    }
    for (uint16_t id = 111; id <= 113 && !c->inv_off; id++) {
        uint16_t off = sunspec_model(s, u, c->base, id, &len);
        if (off) { c->inv_off = off; c->inv_float = 1; }   /* float W@20       */
    }
    uint16_t soc_off = sunspec_model(s, u, c->base, 124, &len);
    if (soc_off && len >= 21) c->soc_off = soc_off;        /* storage / hybrid */
    uint16_t mppt_off = sunspec_model(s, u, c->base, 160, &len);
    if (mppt_off && len >= 8) { c->mppt_off = mppt_off; c->mppt_len = len; }
    for (uint16_t id = 201; id <= 204 && !c->meter_off; id++) {
        uint16_t off = sunspec_model(s, u, c->base, id, &len);
        if (off) c->meter_off = off;
    }
    c->done = true;
}

/* Inverter AC power (W) from the cached model offset. */
static int ss_ac_w(int s, uint8_t u, const ss_cache_t *c, float *out)
{
    if (!c->inv_off) return -2;
    uint16_t w[2];
    if (!c->inv_float) {
        if (mb_read(s, u, MB_FC03, c->inv_off + 12, 2, w)) return -1;  /* W@12, SF@13 */
        *out = apply_sf((float)(int16_t)w[0], (int16_t)w[1]);
    } else {
        if (mb_read(s, u, MB_FC03, c->inv_off + 20, 2, w)) return -1;  /* float W@20 */
        float v = words_to_f32(w[0], w[1]);
        if (!isfinite(v)) return -1;
        *out = v;
    }
    return 0;
}

/* Meter total real power (W) from the cached model offset (W@16, W_SF@20). */
static int ss_meter_w(int s, uint8_t u, const ss_cache_t *c, float *out)
{
    if (!c->meter_off) return -2;
    uint16_t w5[5];
    if (mb_read(s, u, MB_FC03, c->meter_off + 16, 5, w5)) return -1;
    *out = apply_sf((float)(int16_t)w5[0], (int16_t)w5[4]);
    return 0;
}

/* Battery SoC (%) from cached storage model 124 (ChaState@6, ChaState_SF@20). */
static int ss_soc(int s, uint8_t u, const ss_cache_t *c, float *out)
{
    if (!c->soc_off) return -1;
    uint16_t cs, sf;
    if (mb_read(s, u, MB_FC03, c->soc_off + 6, 1, &cs))  return -1;
    if (mb_read(s, u, MB_FC03, c->soc_off + 20, 1, &sf)) return -1;
    float v = apply_sf((float)cs, (int16_t)sf);
    if (v < 0 || v > 100) return -1;            /* SoC must be a sane percent */
    *out = v;
    return 0;
}

/* Solar DC power (W): sum of all MPPT model-160 string modules (DCW × SF). */
static int ss_pv_dc(int s, uint8_t u, const ss_cache_t *c, float *out)
{
    if (!c->mppt_off) return -1;
    uint16_t off = c->mppt_off, len = c->mppt_len;
    uint16_t fixed[8];
    if (mb_read(s, u, MB_FC03, off, 8, fixed)) return -1;
    int16_t dcw_sf = (int16_t)fixed[2];
    uint16_t N = fixed[6];
    if (N == 0 || N > 16) return -1;
    uint16_t blk = (uint16_t)((len - 8) / N);
    if (blk < 12) return -1;
    float sum = 0;
    for (uint16_t i = 0; i < N; i++) {
        uint16_t moff = off + 8 + i * blk;
        uint16_t dcw;
        if (mb_read(s, u, MB_FC03, moff + 11, 1, &dcw)) return -1;  /* DCW @ block+11 */
        sum += (float)(int16_t)dcw;
    }
    *out = apply_sf(sum, dcw_sf);
    return 0;
}

/* --------------------------- per-cycle aggregate ----------------------- */

typedef struct {
    float pv;    bool pv_v;
    float haus;  bool haus_v;
    float netz;  bool netz_v;
    float deyeW, deyeSoc; bool deye_v;       /* Deye via Modbus (SoC + reg590) */
    float deye_ac; bool deye_ac_v;           /* Deye AC power via Eltako meter  */
    float bydW,  bydSoc;  bool byd_v;
    float deye_mppt, deye_ct, deye_load; bool deye_present;
} agg_t;

static int poll_device(int s, const mb_dev_cfg_t *d, int idx, agg_t *a,
                       float *o_pv, float *o_w, float *o_soc)
{
    switch (d->mfr) {
    case MB_MFR_FRONIUS: {
        ss_cache_t *c = &s_ss[idx];
        if (!c->done) {                       /* discover layout once, then cache */
            sunspec_discover(s, d->slave, c);
            if (!c->done) return -1;           /* no SunSpec base yet -> retry     */
        }
        if (d->role == MB_ROLE_INVERTER || d->role == MB_ROLE_BATTERY) {
            /* A unit that carries storage model 124 is a HYBRID (has the BYD
             * battery). On it: AC(103) = solarDC(160) + battery_discharge, so
             *   PV   = solar DC  (pure PV, no battery)
             *   BYD  = AC - solar DC   (+discharge / -charge)
             * A plain string inverter has no 124: PV = AC, no battery. */
            float soc = 0, dc = 0, ac = 0;
            bool hybrid  = (ss_soc(s, d->slave, c, &soc) == 0);
            bool have_dc = (ss_pv_dc(s, d->slave, c, &dc) == 0);
            bool have_ac = (ss_ac_w(s, d->slave, c, &ac) == 0);

            if (hybrid) { a->bydSoc = soc; a->byd_v = true; *o_soc = soc; }

            if (hybrid && have_ac && have_dc) {
                float bw = ac - dc;                 /* battery: +discharge */
                if (!plausible_w(bw)) {
                    ESP_LOGW(TAG, "dev%d: implausible hybrid batt %.0f W -- discarded", idx, bw);
                    return -1;
                }
                a->bydW = bw; a->byd_v = true; *o_w = bw;
                if (d->role == MB_ROLE_INVERTER && plausible_w(dc)) {
                    if (dc < 0) dc = 0;
                    a->pv += dc; a->pv_v = true; *o_pv = dc;   /* PV = solar DC */
                }
            } else if (d->role == MB_ROLE_INVERTER && have_ac) {
                if (!plausible_w(ac)) {
                    ESP_LOGW(TAG, "dev%d: implausible inverter %.0f W -- discarded", idx, ac);
                    return -1;
                }
                if (ac < 0) ac = 0;
                a->pv += ac; a->pv_v = true; *o_pv = ac;       /* pure inverter */
            }
            /* Nothing usable read -> the cached layout may be stale (device
             * swapped/rebooted): drop it so the next poll re-discovers, and
             * report failure so the device shows as disconnected, not @0. */
            if (!hybrid && !have_ac && !have_dc) { c->done = false; return -1; }
            return 0;
        }
        /* meter roles */
        float w = 0;
        if (ss_meter_w(s, d->slave, c, &w)) { c->done = false; return -1; }
        if (!plausible_w(w)) {
            ESP_LOGW(TAG, "dev%d: implausible Fronius meter %.0f W -- discarded", idx, w);
            return -1;
        }
        if (d->role == MB_ROLE_GRID) { a->netz = w; a->netz_v = true; *o_w = w; }
        else                         { a->pv += w; a->pv_v = true; *o_pv = w; }
        return 0;
    }
    case MB_MFR_ELTAKO: {
        uint16_t r[2];
        if (mb_read(s, d->slave, MB_FC04, SDM630_PTOT_ADDR, 2, r)) return -1;
        /* Eltako DSZ15/DSZ16: total active power is a signed int32 in watts
         * (reg 52, high word first), + = import / - = export. Decoding it as a
         * float32 (SDM630 convention) yields nan on export. */
        float w = (float)words_to_i32(r[0], r[1]);
        if (!plausible_w(w)) {
            ESP_LOGW(TAG, "dev%d: implausible Eltako %.0f W -- discarded", idx, w);
            return -1;
        }
        if (d->role == MB_ROLE_GRID) {
            a->netz = w; a->netz_v = true; *o_w = w;
        } else if (d->role == MB_ROLE_DEYE_METER) {
            /* Meter before the AC-coupled Deye inverter. The user's CT reads
             * NEGATIVE when the Deye discharges, so the Deye balance term
             * (+discharge / -charge) is the negated meter reading. */
            a->deye_ac = -w; a->deye_ac_v = true; *o_w = -w;
        } else {  /* PRODMETER -> PV */
            a->pv += w; a->pv_v = true; *o_pv = w;
        }
        return 0;
    }
    case MB_MFR_DEYE:
    default: {
        uint16_t hot[DEYE_HOT_COUNT], c2[DEYE_C2_COUNT];
        if (mb_read(s, d->slave, MB_FC03, DEYE_HOT_START, DEYE_HOT_COUNT, hot)) return -1;
        if (mb_read(s, d->slave, MB_FC03, DEYE_C2_START, DEYE_C2_COUNT, c2))  return -1;
        float soc  = (float)hot[DEYE_BATT_SOC - DEYE_HOT_START];
        float batt = (float)(int16_t)hot[DEYE_BATT_POWER - DEYE_HOT_START];
        float ct   = (float)(int16_t)hot[DEYE_GRID_CT_TOT - DEYE_HOT_START];
        float load = (float)(int16_t)c2[DEYE_LOAD_TOT - DEYE_C2_START];
        float mppt = (float)c2[DEYE_PV1 - DEYE_C2_START] +
                     (float)c2[DEYE_PV2 - DEYE_C2_START];
        a->deyeSoc = soc; a->deyeW = batt; a->deye_v = true;
        a->deye_mppt = mppt; a->deye_ct = ct; a->deye_load = load;
        a->deye_present = true;
        *o_pv = mppt; *o_w = batt; *o_soc = soc;
        return 0;
    }
    }
}

/* --------------------------- poll task --------------------------------- */

/* Parallel poll: one worker task per IP polls its devices CONCURRENTLY with the
 * others (own socket), so a slow inverter datalogger never starves the time-
 * critical grid meter. A separate aggregator combines the per-device
 * contributions into the energy model + UI. User priority order:
 *   1 grid meter -> 2 forward to Deye (RTU) -> 3 read Deye (RTU) -> 4 PV -> 5 BYD
 * The worker serving a grid / Deye-meter device runs at higher task priority;
 * the RTU side (2,3) lives in modbus_rtu.c on its own tasks. */
#define AGG_TICK_MS   800
/* Keep ALL TCP-poll tasks BELOW the LVGL/touch task (esp_lvgl_port default
 * priority 4) so polling never makes the touch UI sluggish. The real-time
 * control path (RTU emulation + Deye read) lives in modbus_rtu.c at priority 5
 * and is unaffected; the grid TCP read only needs to be "fresh enough" (~2 s),
 * which it easily is at this priority (it is I/O-bound and runs whenever LVGL
 * yields). The grid/Deye-meter worker still outranks the PV workers. */
#define MB_PRIO_BG    2    /* PV inverters etc.            */
#define MB_PRIO_CRIT  3    /* grid / Deye-meter owner      */

static agg_t   s_contrib[MB_MAX_DEVICES];   /* last good routed contribution */
static bool    s_valid[MB_MAX_DEVICES];     /* polled OK at least once        */
static uint32_t s_last_ms[MB_MAX_DEVICES];  /* ms of last poll attempt        */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Commit a freshly loaded config into the shared state under the lock so the
 * concurrent workers always snapshot a consistent device list. */
static void reconfigure_apply(void)
{
    mb_dev_cfg_t tmp[MB_MAX_DEVICES];
    int cnt = load_cfg_into(tmp);                  /* NVS I/O -- no lock held */

    bool has_grid = false;
    for (int i = 0; i < cnt; i++)
        if (tmp[i].enabled && tmp[i].role == MB_ROLE_GRID) has_grid = true;

    portENTER_CRITICAL(&s_mux);
    memcpy(s_devs, tmp, sizeof(s_devs));
    s_dev_count = cnt;
    memset(s_contrib, 0, sizeof(s_contrib));
    memset(s_valid,   0, sizeof(s_valid));
    memset(s_last_ms, 0, sizeof(s_last_ms));
    memset(s_ss,      0, sizeof(s_ss));            /* re-discover SunSpec layout */
    s_have_grid_role = has_grid;
    for (int i = 0; i < cnt; i++) {
        memset(&s_live[i], 0, sizeof(s_live[i]));
        strncpy(s_live[i].ip, s_devs[i].ip, sizeof(s_live[i].ip) - 1);
        strncpy(s_live[i].name, s_devs[i].name, sizeof(s_live[i].name) - 1);
        s_live[i].slave = s_devs[i].slave;
        s_live[i].mfr   = s_devs[i].mfr;
        s_live[i].role  = s_devs[i].role;
    }
    s_live_count = cnt;
    /* Drop stale energy + mark grid INVALID so the RTU emulation stops driving
     * the Deye until a fresh read lands. */
    s_grid_valid = false;
    s_st.grid_w = s_st.pv_w = s_st.house_w = 0;
    s_st.deye_w = s_st.deye_soc = s_st.byd_w = s_st.byd_soc = 0;
    portEXIT_CRITICAL(&s_mux);
}

static bool same_ipport(const mb_dev_cfg_t *a, const char *ip, int port)
{
    return a->enabled && a->ip[0] &&
           strcmp(a->ip, ip) == 0 && (a->port ? a->port : 502) == port;
}

/* One worker per device slot: active only when it owns its IP (lowest enabled
 * index with that IP). It polls every enabled device sharing the IP on its own
 * interval, over a single socket (gateways like the ZGW allow one connection). */
static void worker_task(void *arg)
{
    int  slot     = (int)(intptr_t)arg;
    int  cur_prio = MB_PRIO_BG;
    int  sk       = -1;            /* PERSISTENT socket -- reused across polls.   */
    char sk_ip[32] = ""; int sk_port = 0;  /* what `sk` is connected to.          */

    for (;;) {
        mb_dev_cfg_t devs[MB_MAX_DEVICES];
        int cnt;
        portENTER_CRITICAL(&s_mux);
        memcpy(devs, s_devs, sizeof(devs));
        cnt = s_dev_count;
        portEXIT_CRITICAL(&s_mux);

        bool idle = (slot >= cnt || !devs[slot].enabled || devs[slot].ip[0] == '\0');
        const char *ip = idle ? "" : devs[slot].ip;
        int port = idle ? 0 : (devs[slot].port ? devs[slot].port : 502);

        /* Owner = lowest enabled index with this IP:port; others stay idle. */
        if (!idle)
            for (int j = 0; j < slot; j++)
                if (same_ipport(&devs[j], ip, port)) { idle = true; break; }

        /* Drop the held socket if we're going idle or the target IP changed. */
        if (sk >= 0 && (idle || strcmp(sk_ip, ip) != 0 || sk_port != port)) {
            close(sk); sk = -1;
        }
        if (idle) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        /* Priority 1/2: a worker serving a grid / Deye-meter device runs higher. */
        bool critical = false;
        for (int j = 0; j < cnt; j++)
            if (same_ipport(&devs[j], ip, port) &&
                (devs[j].role == MB_ROLE_GRID || devs[j].role == MB_ROLE_DEYE_METER))
                critical = true;
        int want_prio = critical ? MB_PRIO_CRIT : MB_PRIO_BG;
        if (want_prio != cur_prio) { vTaskPrioritySet(NULL, want_prio); cur_prio = want_prio; }

        /* Which same-IP devices are due now? */
        uint32_t now = now_ms();
        int due[MB_MAX_DEVICES], ndue = 0, fastest = MB_MAX_POLL_MS;
        for (int j = 0; j < cnt; j++) {
            if (!same_ipport(&devs[j], ip, port)) continue;
            uint16_t iv = devs[j].poll_ms ? devs[j].poll_ms : MB_DEFAULT_POLL_MS;
            if (iv < fastest) fastest = iv;
            uint32_t last; bool valid;
            portENTER_CRITICAL(&s_mux); last = s_last_ms[j]; valid = s_valid[j]; portEXIT_CRITICAL(&s_mux);
            if (valid && (uint32_t)(now - last) < iv) continue;
            due[ndue++] = j;
        }
        int nap = fastest < 250 ? fastest : 250;   /* re-check due devices ~>=4x/s */
        if (ndue == 0) { vTaskDelay(pdMS_TO_TICKS(nap > 0 ? nap : 50)); continue; }

        /* Connect only when we don't already hold a socket. Keeping it open is
         * essential: Fronius dataloggers are flaky with connect/close churn and
         * gateways (ZGW) allow only one connection. */
        if (sk < 0) {
            int tmo   = devs[slot].timeout_ms ? devs[slot].timeout_ms : MB_DEFAULT_TIMEOUT_MS;
            int io_ms = tmo < 2000 ? 2000 : tmo;
            sk = connect_timeout(ip, port, tmo, io_ms);     /* I/O -- no lock held */
            strncpy(sk_ip, ip, sizeof(sk_ip) - 1); sk_ip[sizeof(sk_ip) - 1] = '\0';
            sk_port = port;
        }

        bool trouble = (sk < 0);    /* connect failed -> back off, don't storm */
        for (int k = 0; k < ndue; k++) {
            int j = due[k];
            portENTER_CRITICAL(&s_mux); s_last_ms[j] = now; portEXIT_CRITICAL(&s_mux);

            if (sk < 0) {
                portENTER_CRITICAL(&s_mux);
                s_st.err_count++; s_live[j].connected = false;
                portEXIT_CRITICAL(&s_mux);
                continue;
            }

            agg_t c = {0};
            float pv = 0, w = 0, soc = 0;
            int r = poll_device(sk, &devs[j], j, &c, &pv, &w, &soc);   /* I/O -- no lock */
            uint32_t fnow = now_ms();

            portENTER_CRITICAL(&s_mux);
            if (r != 0) {
                s_st.err_count++; s_live[j].connected = false;
            } else {
                s_contrib[j] = c; s_valid[j] = true;
                s_live[j].connected = true; s_live[j].pv_w = pv; s_live[j].w = w; s_live[j].soc = soc;
                /* Keep the control-critical grid value fresh IMMEDIATELY (the RTU
                 * emulation reads it), independent of the aggregator's cadence. */
                if (c.netz_v) { s_st.grid_w = c.netz; s_grid_ms = fnow; s_grid_valid = true; }
                else if (!s_have_grid_role && c.deye_present) {
                    s_st.grid_w = c.deye_ct; s_grid_ms = fnow; s_grid_valid = true;
                }
            }
            portEXIT_CRITICAL(&s_mux);

            if (r != 0) {
                ESP_LOGW(TAG, "read failed %s id%u (%s/%s) -- reconnecting", ip,
                         devs[j].slave, modbus_tcp_mfr_name(devs[j].mfr),
                         modbus_tcp_role_name(devs[j].role));
                close(sk); sk = -1;        /* drop on error; reconnect next round */
                trouble = true;
                break;                     /* don't hammer the rest on a dead link */
            }
        }

        /* Back off hard after a failed connect/read so a non-responding inverter
         * (e.g. a Fronius datalogger still holding its old slot after a reboot)
         * is retried every few seconds, not several times a second. */
        if (trouble) vTaskDelay(pdMS_TO_TICKS(3000));
        else         vTaskDelay(pdMS_TO_TICKS(nap > 0 ? nap : 50));
    }
}

/* Aggregator: applies reconfig, then every AGG_TICK_MS rebuilds the energy
 * model from the per-device contributions the workers filled, and updates the UI. */
static void agg_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_reconf) { s_reconf = false; reconfigure_apply(); }

        agg_t contrib[MB_MAX_DEVICES]; bool valid[MB_MAX_DEVICES]; int cnt;
        bool rtu_dv; float rtu_dw, rtu_ds; uint32_t rtu_dms;
        portENTER_CRITICAL(&s_mux);
        cnt = s_dev_count;
        memcpy(contrib, s_contrib, sizeof(contrib));
        memcpy(valid,   s_valid,   sizeof(valid));
        rtu_dv = s_rtu_deye_valid; rtu_dw = s_rtu_deye_w; rtu_ds = s_rtu_deye_soc; rtu_dms = s_rtu_deye_ms;
        portEXIT_CRITICAL(&s_mux);

        agg_t a = {0};
        bool has_any = false;
        for (int i = 0; i < cnt; i++) {
            if (!valid[i]) continue;
            has_any = true;
            agg_t *c = &contrib[i];
            a.pv += c->pv; if (c->pv_v) a.pv_v = true;
            if (c->netz_v) { a.netz = c->netz; a.netz_v = true; }
            if (c->byd_v)  { a.bydW = c->bydW; a.bydSoc = c->bydSoc; a.byd_v = true; }
            if (c->deye_v) { a.deyeW = c->deyeW; a.deyeSoc = c->deyeSoc; a.deye_v = true; }
            if (c->deye_ac_v) { a.deye_ac = c->deye_ac; a.deye_ac_v = true; }
            if (c->deye_present) {
                a.deye_mppt = c->deye_mppt; a.deye_ct = c->deye_ct;
                a.deye_load = c->deye_load; a.deye_present = true;
            }
        }

        /* RTU Deye (modbus_rtu.c) is the SoC source and overrides reg-590; only
         * while FRESH, else it would ghost after the Deye-read bus is turned off. */
        if (rtu_dv && (uint32_t)(now_ms() - rtu_dms) <= MB_RTU_DEYE_MAX_AGE_MS) {
            a.deyeW = rtu_dw; a.deyeSoc = rtu_ds; a.deye_v = true;
        }

        float deye_pw   = a.deye_ac_v ? a.deye_ac : (a.deye_v ? a.deyeW : 0.0f);
        bool  deye_pw_v = a.deye_ac_v || a.deye_v;

        if (!a.pv_v   && a.deye_present) { a.pv = a.deye_mppt; a.pv_v = true; }
        if (!a.netz_v && a.deye_present) { a.netz = a.deye_ct;  a.netz_v = true; }

        if (a.pv_v && a.netz_v) {
            float batt = (a.byd_v ? a.bydW : 0.0f) + (deye_pw_v ? deye_pw : 0.0f);
            float h = a.pv + batt + a.netz;
            if (h < 0) h = 0;
            a.haus = h; a.haus_v = true;
        } else if (a.deye_present) {
            a.haus = a.deye_load; a.haus_v = true;   /* pure-Deye fallback */
        }

        int connected = 0;
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < cnt; i++) if (s_live[i].connected) connected++;
        if (has_any) {
            s_st.poll_count++;
            /* Second plausibility gate: the primary filter is in poll_device();
             * this catches anything that slips through (e.g. RTU-fed values). */
            if (a.pv_v   && plausible_w(a.pv))    s_st.pv_w    = a.pv;
            else if (a.pv_v)   ESP_LOGW(TAG, "agg: implausible pv %.0f W skipped",   a.pv);
            if (a.haus_v && plausible_w(a.haus))   s_st.house_w = a.haus;
            else if (a.haus_v) ESP_LOGW(TAG, "agg: implausible haus %.0f W skipped", a.haus);
            if (a.netz_v && plausible_w(a.netz))   s_st.grid_w  = a.netz;
            else if (a.netz_v) ESP_LOGW(TAG, "agg: implausible netz %.0f W skipped", a.netz);
            if (deye_pw_v) {
                if (plausible_w(deye_pw)) {
                    s_st.deye_w = deye_pw;
                    if (a.deye_v) s_st.deye_soc = a.deyeSoc;
                } else { ESP_LOGW(TAG, "agg: implausible deye %.0f W skipped", deye_pw); }
            } else { s_st.deye_w = 0; s_st.deye_soc = 0; }
            if (a.byd_v) {
                if (plausible_w(a.bydW)) { s_st.byd_w = a.bydW; s_st.byd_soc = a.bydSoc; }
                else ESP_LOGW(TAG, "agg: implausible byd %.0f W skipped", a.bydW);
            }
        }
        s_st.dev_count = cnt;
        s_st.connected = connected;
        portEXIT_CRITICAL(&s_mux);

        /* SLS export guard: during forced discharge, throttle reg143 (sell power)
         * so that grid export never exceeds 90% of the fuse rating.
         * Math: max_export = SLS_A × 3 × 230 V × 0.9
         * If export > max: throttle = user_setpoint − overshoot
         * If export ≤ max: restore to user_setpoint.
         * Dead-band ±200 W avoids constant RTU writes from measurement noise. */
        if (has_any && a.netz_v && deye_ctrl_get_mode() == DEYE_MODE_FORCE_DISCHARGE) {
            uint8_t sls_a = nvs_store_get_sls_a();
            if (sls_a > 0) {
                float max_export_w = (float)sls_a * 3.0f * 230.0f * 0.9f;
                float export_w     = -a.netz;   /* netz negative = export; we want positive */
                int   user_pw      = deye_ctrl_get_user_power();
                int   target_pw;
                if (export_w > max_export_w) {
                    float excess = export_w - max_export_w;
                    target_pw = (int)((float)user_pw - excess);
                    if (target_pw < 1000) target_pw = 1000;
                } else {
                    target_pw = user_pw;
                }
                if (abs(target_pw - deye_ctrl_get_power()) > 200) {
                    deye_ctrl_set_throttled(target_pw);
                    if (export_w > max_export_w)
                        ESP_LOGW(TAG, "SLS guard: export %.0f W > limit %.0f W (SLS %uA) -- throttle → %d W",
                                 export_w, max_export_w, (unsigned)sls_a, target_pw);
                    else
                        ESP_LOGI(TAG, "SLS guard: export %.0f W OK -- restore → %d W",
                                 export_w, target_pw);
                }
            }
        }

        if (has_any && connected && app_lvgl_lock(100)) {
            if (a.pv_v)    ui_flow_set_pv(a.pv / 1000.0f);     else ui_flow_clear_pv();
            if (a.haus_v)  ui_flow_set_house(a.haus / 1000.0f); else ui_flow_clear_house();
            if (a.netz_v)  ui_flow_set_grid(a.netz / 1000.0f); else ui_flow_clear_grid();
            if (deye_pw_v) ui_flow_set_deye(deye_pw / 1000.0f, a.deyeSoc); else ui_flow_clear_deye();
            if (a.byd_v)   ui_flow_set_byd(a.bydW / 1000.0f, a.bydSoc); else ui_flow_clear_byd();
            app_lvgl_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(AGG_TICK_MS));
    }
}

/* --------------------------- public API -------------------------------- */

const char *modbus_tcp_mfr_name(uint8_t mfr)
{
    switch (mfr) {
    case MB_MFR_FRONIUS: return "Fronius";
    case MB_MFR_DEYE:    return "Deye";
    case MB_MFR_ELTAKO:  return "Eltako";
    default:             return "?";
    }
}

const char *modbus_tcp_role_name(uint8_t role)
{
    switch (role) {
    case MB_ROLE_GRID:      return "Netz-Zaehler";
    case MB_ROLE_PRODMETER: return "Erzeugungs-Zaehler";
    case MB_ROLE_INVERTER:  return "Wechselrichter";
    case MB_ROLE_BATTERY:   return "Batterie";
    case MB_ROLE_DEYE_METER:return "Deye-AC-Zaehler";
    default:                return "?";
    }
}

esp_err_t modbus_tcp_start(void)
{
    /* One worker per device slot (each polls its own IP concurrently) + one
     * aggregator that builds the energy model and drives the UI. */
    for (int i = 0; i < MB_MAX_DEVICES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "mb_w%d", i);
        /* Pinned to core 0: LVGL/touch owns core 1 (see app_lvgl_start), so the
         * poll workers never compete with the UI for CPU. */
        if (xTaskCreatePinnedToCore(worker_task, name, 4096, (void *)(intptr_t)i,
                                    MB_PRIO_BG, NULL, 0) != pdPASS) {
            return ESP_FAIL;
        }
    }
    if (xTaskCreatePinnedToCore(agg_task, "mb_agg", 5120, NULL, MB_PRIO_BG, NULL, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void modbus_tcp_reconfigure(void) { s_reconf = true; }

void modbus_tcp_get_status(modbus_tcp_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

bool modbus_tcp_grid_w_fresh(float *out_w, uint32_t max_age_ms)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_grid_valid && (uint32_t)(now - s_grid_ms) <= max_age_ms) {
        if (out_w) *out_w = s_st.grid_w;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

int modbus_tcp_get_device_live(mb_dev_live_t *out, int max)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_live_count < max ? s_live_count : max;
    for (int i = 0; i < n; i++) out[i] = s_live[i];
    portEXIT_CRITICAL(&s_mux);
    return n;
}

int modbus_tcp_get_devices(mb_dev_cfg_t *out, int max)
{
    size_t len = sizeof(s_devs);
    mb_dev_cfg_t tmp[MB_MAX_DEVICES];
    memset(tmp, 0, sizeof(tmp));
    int n = 0;
    if (nvs_store_get_mb_devices(tmp, &len) == ESP_OK) {
        n = (int)(len / sizeof(mb_dev_cfg_t));
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    return n;
}

esp_err_t modbus_tcp_set_devices(const mb_dev_cfg_t *list, int count)
{
    if (count < 0) count = 0;
    if (count > MB_MAX_DEVICES) count = MB_MAX_DEVICES;
    esp_err_t e = nvs_store_set_mb_devices(list, (size_t)count * sizeof(mb_dev_cfg_t));
    if (e == ESP_OK) modbus_tcp_reconfigure();
    return e;
}

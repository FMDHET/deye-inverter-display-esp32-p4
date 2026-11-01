#include "fronius_meter.h"
#include "modbus_rtu.h"
#include "nvs_store.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"

static const char *TAG = "frm";

/* ------------------------------ register image -------------------------
 * Everything below is the Fronius Smart Meter as the Symo Hybrid knows it,
 * taken over unchanged from The Bridge (fronius.cpp), where each entry is
 * backed by an RS485 recording of the real meter. Do not "tidy" values whose
 * meaning is unknown -- the inverter checks them during its scan. */

#define FC_READ_HOLDING   0x03
#define EX_ILLEGAL_FUNC   0x01
#define EX_ILLEGAL_ADDR   0x02
#define EX_ILLEGAL_VALUE  0x03
#define MAX_READ_REGS     125

typedef enum {
    Q_U_AVG, Q_U_LL_AVG, Q_P_TOT, Q_S_TOT, Q_Q_TOT, Q_PF_TOT, Q_ZERO, Q_FREQ,
    Q_U_LL, Q_U, Q_I, Q_P, Q_S, Q_Q, Q_PF,
    Q_IMP_KWH, Q_IMP_WH, Q_EXP_KWH, Q_EXP_WH,
} quantity_t;

typedef struct {
    uint16_t   addr;
    quantity_t q;
    int8_t     ph;        /* 0..2 for phase quantities, -1 otherwise */
    int16_t    factor;    /* raw = value * factor                    */
} int32_reg_t;

/* int32, two's complement, LOW word first (CDAB) -- unlike the rest of the
 * Modbus world, and unlike the SunSpec map, which the Fronius does NOT accept
 * as a smart meter. */
static const int32_reg_t INT32_REGS[] = {
    {   0, Q_U,        0,   10 },   /* U L1-N, asked once during the scan */
    { 258, Q_U_AVG,   -1,   10 },
    { 260, Q_U_LL_AVG,-1,   10 },
    { 262, Q_P_TOT,   -1,   10 },
    { 264, Q_S_TOT,   -1,   10 },
    { 266, Q_Q_TOT,   -1,   10 },
    { 268, Q_PF_TOT,  -1, 1000 },
    { 270, Q_ZERO,    -1,    1 },   /* meaning unknown, always 0 on the real meter */
    { 272, Q_FREQ,    -1,   10 },
    { 286, Q_U_LL,     0,   10 }, { 288, Q_U, 0, 10 }, { 290, Q_I, 0, 1000 },
    { 292, Q_P,        0,   10 }, { 294, Q_S, 0, 10 }, { 296, Q_Q, 0, 10 },
    { 298, Q_PF,       0, 1000 },
    { 300, Q_U_LL,     1,   10 }, { 302, Q_U, 1, 10 }, { 304, Q_I, 1, 1000 },
    { 306, Q_P,        1,   10 }, { 308, Q_S, 1, 10 }, { 310, Q_Q, 1, 10 },
    { 312, Q_PF,       1, 1000 },
    { 314, Q_U_LL,     2,   10 }, { 316, Q_U, 2, 10 }, { 318, Q_I, 2, 1000 },
    { 320, Q_P,        2,   10 }, { 322, Q_S, 2, 10 }, { 324, Q_Q, 2, 10 },
    { 326, Q_PF,       2, 1000 },
    { 1024, Q_IMP_KWH, -1,   1 }, { 1026, Q_IMP_WH, -1, 1 },
    { 1028, Q_ZERO,    -1,   1 }, { 1030, Q_ZERO,   -1, 1 },   /* reactive energy: none */
    { 1032, Q_EXP_KWH, -1,   1 }, { 1034, Q_EXP_WH, -1, 1 },
    { 1036, Q_ZERO,    -1,   1 }, { 1038, Q_ZERO,   -1, 1 },
};

typedef struct { uint16_t addr, val; } fixed_reg_t;

/* Fixed registers of the start sequence. The serial number sits in between
 * (20480..20488) and comes from the configuration, see reg_value(). */
static const fixed_reg_t FIXED_REGS[] = {
    {    11, 731 },                   /* type code -- must be exactly this */
    {   770, 1 }, {  771, 3 },
    {  4098, 0 }, { 4355, 1 }, { 4356, 0 }, { 4357, 14 },
    { 20496, 2020 },
};

/* Serial number: one ASCII character per register, then the number the
 * Fronius compares, HIGH word first (real meter: "158027U", 0x3174:0x01DF =
 * 829686239) -- unlike the measurement int32s. */
#define SERIAL_TXT_REG  20480
#define SERIAL_HI_REG   20487
#define SERIAL_LO_REG   20488

/* Asked during the scan and refused by the real meter (Exception 02). They are
 * not special-cased -- they are simply not in the image -- but refusing them
 * is expected and must not be logged as a surprise. */
static const uint16_t EXPECTED_REJECT[] = { 768, 1706, 40000, 50000 };

/* ------------------------------ meter values --------------------------- */

typedef struct {
    float  u[3], u_ll[3], i[3], p[3], s[3], q[3], pf[3];
    float  p_tot, s_tot, q_tot, pf_tot, freq;
    double imp_kwh, exp_kwh;
    uint32_t serial;
    char     serial_txt[8];
} frm_values_t;

/* Persisted counters (Wh). Appended fields only. */
typedef struct {
    double imp_wh, exp_wh;
} frm_energy_t;

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static fronius_meter_status_t s_st;
static frm_energy_t   s_e;             /* only touched by the bridge task */
static bool           s_e_loaded;
static frm_energy_t   s_e_saved;
static int64_t        s_e_saved_us;
static int64_t        s_last_round_us; /* time of the Deye round last integrated */
static bool           s_was_fresh = true;
static int64_t        s_warn_us;       /* rate limit for "unknown request" */
static int64_t        s_last_req_us;   /* last request, under s_mux (0 = never) */

#define ENERGY_SAVE_US  (15LL * 60 * 1000000)   /* NVS wear: at most every 15 min */
#define ROUND_GAP_MAX_US (15LL * 1000000)       /* longer gap -> do not bridge it */
#define WARN_EVERY_US   (10LL * 1000000)

static float pf_of(float p, float s)
{
    if (s < 1.0f) return 1.0f;
    float pf = p / s;
    return pf > 1.0f ? 1.0f : (pf < -1.0f ? -1.0f : pf);
}

/* U_ab from two phase voltages 120 degrees apart. */
static float ll_of(float ua, float ub)
{
    return sqrtf(ua * ua + ub * ub + ua * ub);
}

/* Build the meter's view from the Deye's own AC measurement. False when the
 * values are not fresh enough to be served. */
static bool snapshot(frm_values_t *m)
{
    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    if (!l.valid || !l.online || !(l.blocks & MB_DEYE_BLK_OUT) || l.age_ms > FRM_STALE_MS)
        return false;

    memset(m, 0, sizeof(*m));
    for (int k = 0; k < 3; k++) {
        /* In standby the Deye reports 0 V on its inverter side; the grid side
         * still has the real voltage, and a meter never shows 0 V on a live
         * line. */
        float u = l.inv_v[k] > 50.0f ? l.inv_v[k] : l.grid_v[k];
        float p = l.inv_p[k];                      /* + draws, - delivers */
        float i = fabsf(l.inv_i[k]);
        float s = fmaxf(u * i, fabsf(p));
        m->u[k]  = u;
        m->p[k]  = p;
        m->i[k]  = p < 0 ? -i : i;                 /* sign follows the power */
        m->s[k]  = s;
        m->q[k]  = sqrtf(fmaxf(0.0f, s * s - p * p));
        m->pf[k] = pf_of(p, s);
        m->p_tot += p; m->s_tot += s; m->q_tot += m->q[k];
    }
    for (int k = 0; k < 3; k++) m->u_ll[k] = ll_of(m->u[k], m->u[(k + 1) % 3]);
    m->pf_tot = pf_of(m->p_tot, hypotf(m->p_tot, m->q_tot));
    m->freq   = l.inv_freq > 40.0f ? l.inv_freq : l.grid_freq;
    m->imp_kwh = s_e.imp_wh / 1000.0;
    m->exp_kwh = s_e.exp_wh / 1000.0;

    mb_rtu_cfg_t c;
    modbus_rtu_get_cfg(&c);           /* clamp_cfg() guarantees valid values */
    m->serial = c.frm_serial;
    memcpy(m->serial_txt, c.frm_serial_txt, sizeof(m->serial_txt));
    return true;
}

static double qty(const int32_reg_t *r, const frm_values_t *m)
{
    int k = r->ph < 0 ? 0 : r->ph;
    switch (r->q) {
    case Q_U_AVG:    return (m->u[0] + m->u[1] + m->u[2]) / 3.0;
    case Q_U_LL_AVG: return (m->u_ll[0] + m->u_ll[1] + m->u_ll[2]) / 3.0;
    case Q_P_TOT:    return m->p_tot;
    case Q_S_TOT:    return m->s_tot;
    case Q_Q_TOT:    return m->q_tot;
    case Q_PF_TOT:   return m->pf_tot;
    case Q_ZERO:     return 0;
    case Q_FREQ:     return m->freq;
    case Q_U_LL:     return m->u_ll[k];
    case Q_U:        return m->u[k];
    case Q_I:        return m->i[k];
    case Q_P:        return m->p[k];
    case Q_S:        return m->s[k];
    case Q_Q:        return m->q[k];
    case Q_PF:       return m->pf[k];
    /* Whole kWh plus a 0..999 Wh rest, both rounded DOWN so the pair never
     * runs ahead of the real count -- the Fronius builds differences. */
    case Q_IMP_KWH:  return floor(m->imp_kwh);
    case Q_IMP_WH:   return fmin(999.0, floor((m->imp_kwh - floor(m->imp_kwh)) * 1000.0));
    case Q_EXP_KWH:  return floor(m->exp_kwh);
    case Q_EXP_WH:   return fmin(999.0, floor((m->exp_kwh - floor(m->exp_kwh)) * 1000.0));
    }
    return 0;
}

/* 16-bit content of one register; false = the real meter does not know it. */
static bool reg_value(uint16_t a, const frm_values_t *m, uint16_t *v)
{
    if (a >= SERIAL_TXT_REG && a < SERIAL_TXT_REG + FRM_SERIAL_TXT_LEN) {
        *v = (uint8_t)m->serial_txt[a - SERIAL_TXT_REG];
        return true;
    }
    if (a == SERIAL_HI_REG) { *v = (uint16_t)(m->serial >> 16); return true; }
    if (a == SERIAL_LO_REG) { *v = (uint16_t)m->serial;         return true; }
    for (size_t n = 0; n < sizeof(INT32_REGS) / sizeof(INT32_REGS[0]); n++) {
        const int32_reg_t *r = &INT32_REGS[n];
        if (a != r->addr && a != r->addr + 1) continue;
        double x = qty(r, m) * r->factor;
        if (!isfinite(x)) x = 0;
        x = fmax((double)INT32_MIN, fmin((double)INT32_MAX, x));
        uint32_t raw = (uint32_t)(int32_t)llround(x);
        *v = (a == r->addr) ? (uint16_t)raw : (uint16_t)(raw >> 16);   /* low word first */
        return true;
    }
    for (size_t n = 0; n < sizeof(FIXED_REGS) / sizeof(FIXED_REGS[0]); n++) {
        if (FIXED_REGS[n].addr == a) { *v = FIXED_REGS[n].val; return true; }
    }
    return false;
}

static bool expected_reject(uint16_t start)
{
    for (size_t n = 0; n < sizeof(EXPECTED_REJECT) / sizeof(EXPECTED_REJECT[0]); n++)
        if (EXPECTED_REJECT[n] == start) return true;
    return false;
}

static int exception(uint8_t fc, uint8_t code, uint8_t *out)
{
    out[0] = (uint8_t)(fc | 0x80);
    out[1] = code;
    portENTER_CRITICAL(&s_mux); s_st.exceptions++; portEXIT_CRITICAL(&s_mux);
    return 2;
}

/* ------------------------------ public --------------------------------- */

int fronius_meter_answer(const uint8_t *pdu, int len, uint8_t *out, int out_max,
                         uint32_t client_ip)
{
    if (len < 1) return 0;
    uint8_t fc = pdu[0];
    /* A read is exactly 5 bytes; anything else at FC03 is not a request. */
    if (fc == FC_READ_HOLDING && len != 5) return 0;

    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_st.requests++;
    s_st.client_ip = client_ip;
    s_last_req_us  = now;
    portEXIT_CRITICAL(&s_mux);

    frm_values_t m;
    if (!snapshot(&m)) {
        /* Silence, like a meter without a measurement: the Fronius then marks
         * this meter as failed instead of booking a frozen value. */
        portENTER_CRITICAL(&s_mux); s_st.silent++; portEXIT_CRITICAL(&s_mux);
        return 0;
    }

    if (fc != FC_READ_HOLDING) return exception(fc, EX_ILLEGAL_FUNC, out);
    uint16_t start = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t cnt   = (uint16_t)((pdu[3] << 8) | pdu[4]);
    if (cnt < 1 || cnt > MAX_READ_REGS || 2 + 2 * cnt > out_max)
        return exception(fc, EX_ILLEGAL_VALUE, out);

    out[0] = FC_READ_HOLDING;
    out[1] = (uint8_t)(2 * cnt);
    for (uint16_t n = 0; n < cnt; n++) {
        uint16_t v;
        /* Like the real meter: one unknown register turns the whole request
         * into Exception 02. */
        if (!reg_value((uint16_t)(start + n), &m, &v)) {
            if (!expected_reject(start)) {
                if (now - s_warn_us > WARN_EVERY_US) {
                    s_warn_us = now;
                    struct in_addr ia = { .s_addr = client_ip };
                    ESP_LOGW(TAG, "%s: unknown request FC03 %u (+%u) -> Exception 02",
                             inet_ntoa(ia), (unsigned)start, (unsigned)cnt);
                }
            }
            return exception(fc, EX_ILLEGAL_ADDR, out);
        }
        out[2 + 2 * n]     = (uint8_t)(v >> 8);
        out[2 + 2 * n + 1] = (uint8_t)v;
    }
    portENTER_CRITICAL(&s_mux); s_st.answered++; portEXIT_CRITICAL(&s_mux);
    return 2 + 2 * cnt;
}

void fronius_meter_tick(void)
{
    mb_rtu_cfg_t c;
    modbus_rtu_get_cfg(&c);
    int64_t now = esp_timer_get_time();

    if (!s_e_loaded) {
        s_e_loaded = true;
        if (nvs_store_get_frm_energy(&s_e, sizeof(s_e)) != ESP_OK ||
            !isfinite(s_e.imp_wh) || !isfinite(s_e.exp_wh) ||
            s_e.imp_wh < 0 || s_e.exp_wh < 0) {
            memset(&s_e, 0, sizeof(s_e));
        }
        s_e_saved    = s_e;
        s_e_saved_us = now;
    }

    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    bool fresh = l.valid && l.online && (l.blocks & MB_DEYE_BLK_OUT) &&
                 l.age_ms <= FRM_STALE_MS;

    if (c.frm_enabled && fresh) {
        /* Integrate once per Deye round, over the time since the previous one.
         * A gap longer than ROUND_GAP_MAX_US (bus outage, restart) is not
         * bridged with a guess: that energy is simply not counted. */
        int64_t round_us = now - (int64_t)l.age_ms * 1000;
        int64_t dt = round_us - s_last_round_us;
        if (s_last_round_us == 0 || dt > ROUND_GAP_MAX_US) {
            s_last_round_us = round_us;
        } else if (dt > 300000) {
            double wh = (double)l.inv_total * (double)dt / 3.6e9;
            if (wh > 0) s_e.imp_wh += wh;          /* Deye drew from the house */
            else        s_e.exp_wh -= wh;          /* Deye fed into the house  */
            s_last_round_us = round_us;
        }
    } else {
        s_last_round_us = 0;
    }

    if (c.frm_enabled && fresh != s_was_fresh) {
        if (fresh) ESP_LOGI(TAG, "Deye values fresh again -- producer meter answers");
        else       ESP_LOGW(TAG, "Deye values stale -- producer meter stays silent");
    }
    s_was_fresh = fresh;

    if (now - s_e_saved_us > ENERGY_SAVE_US &&
        (s_e.imp_wh != s_e_saved.imp_wh || s_e.exp_wh != s_e_saved.exp_wh)) {
        if (nvs_store_set_frm_energy(&s_e, sizeof(s_e)) == ESP_OK) s_e_saved = s_e;
        s_e_saved_us = now;
    }

    portENTER_CRITICAL(&s_mux);
    s_st.enabled = c.frm_enabled != 0;
    s_st.unit    = c.frm_unit;
    s_st.serial  = c.frm_serial;
    memcpy(s_st.serial_txt, c.frm_serial_txt, sizeof(s_st.serial_txt));
    s_st.fresh   = fresh;
    for (int k = 0; k < 3; k++) s_st.p[k] = fresh ? l.inv_p[k] : 0;
    s_st.p_total    = fresh ? l.inv_total : 0;
    s_st.import_kwh = s_e.imp_wh / 1000.0;
    s_st.export_kwh = s_e.exp_wh / 1000.0;
    portEXIT_CRITICAL(&s_mux);
}

void fronius_meter_get_status(fronius_meter_status_t *out)
{
    if (!out) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    out->age_ms = s_last_req_us ? (uint32_t)((now - s_last_req_us) / 1000) : 0;
    if (s_last_req_us && out->age_ms == 0) out->age_ms = 1;
    portEXIT_CRITICAL(&s_mux);
}

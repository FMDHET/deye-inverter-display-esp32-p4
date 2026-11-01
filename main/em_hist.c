#include "em_hist.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR: buffers in PSRAM, internal RAM is for WiFi/TLS */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *EH_TAG = "em_hist";

#define EH_SLOT_S     900
#define EH_REC_SIZE   32
#define EH_SECTOR     4096
#define EH_PER_SECTOR (EH_SECTOR / EH_REC_SIZE)
#define EH_MAGIC      0x4845u              /* "EH" */
#define EH_VER        1
#define EH_LABEL      "emhist"
#define EH_SUBTYPE    0x41
#define EH_OFFSET     0xB00000
#define EH_SIZE       0x100000
#define EH_U16_NONE   0xFFFFu
#define EH_I16_NONE   INT16_MIN
#define EH_LOOKBACK_S 86400                /* a range read stops this far before t0 */

/* One record in flash. The checksum covers everything before it; a record
 * cut short by a power loss mid-write fails it and is skipped. */
typedef struct {
    uint16_t magic;
    uint8_t  ver, flags;
    uint32_t seq;                /* write order, the newest has the highest  */
    uint32_t t;                  /* slot start, unix                         */
    uint16_t deye_soc, byd_soc;  /* % x 10                                   */
    int16_t  deye_bat_w, grid_w, pv_w, house_w;
    int16_t  price;              /* ct x 100                                 */
    uint8_t  act, mode;
    uint32_t sum;                /* FNV-1a over all of the above             */
} eh_rec_t;
_Static_assert(sizeof(eh_rec_t) == EH_REC_SIZE, "record fills its slot");

/* ------------------------------ I/O hooks -------------------------------
 * As in load_hist.c: flash sits behind three pointers, the host suite puts
 * RAM there. */
typedef struct {
    esp_err_t (*read)(size_t off, void *buf, size_t len);
    esp_err_t (*write)(size_t off, const void *buf, size_t len);
    esp_err_t (*erase)(size_t off, size_t len);
    size_t    size;
} eh_io_t;

#ifndef EM_HIST_HOST_TEST
#include "esp_flash.h"
#include "esp_partition.h"

static const esp_partition_t *s_eh_part;
static esp_err_t eh_p_read(size_t o, void *b, size_t l)        { return esp_partition_read(s_eh_part, o, b, l); }
static esp_err_t eh_p_write(size_t o, const void *b, size_t l) { return esp_partition_write(s_eh_part, o, b, l); }
static esp_err_t eh_p_erase(size_t o, size_t l)                { return esp_partition_erase_range(s_eh_part, o, l); }
static eh_io_t s_eh_part_io = { eh_p_read, eh_p_write, eh_p_erase, 0 };
static eh_io_t *s_eh_io;

static esp_err_t eh_io_open(void)
{
    if (s_eh_io) return ESP_OK;
    s_eh_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, EH_SUBTYPE, EH_LABEL);
    if (!s_eh_part) {
        esp_err_t e = esp_partition_register_external(esp_flash_default_chip, EH_OFFSET, EH_SIZE,
                                                      EH_LABEL, ESP_PARTITION_TYPE_DATA, EH_SUBTYPE, &s_eh_part);
        if (e != ESP_OK) {
            ESP_LOGE(EH_TAG, "Flashbereich 0x%x nicht verfügbar: %s", EH_OFFSET, esp_err_to_name(e));
            return e;
        }
        ESP_LOGI(EH_TAG, "Flashbereich 0x%x (%u kB) zur Laufzeit eingebunden", EH_OFFSET, EH_SIZE / 1024);
    }
    s_eh_part_io.size = s_eh_part->size;
    s_eh_io = &s_eh_part_io;
    return ESP_OK;
}
#else
static eh_io_t *s_eh_io;                     /* the host suite installs its fake */
static esp_err_t eh_io_open(void) { return s_eh_io ? ESP_OK : ESP_ERR_NOT_FOUND; }
#endif

/* ------------------------------- state ---------------------------------- */

static SemaphoreHandle_t s_eh_lock;
static portMUX_TYPE      s_eh_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool eh_lock(void)
{
    portENTER_CRITICAL(&s_eh_init_mux);
    if (!s_eh_lock) s_eh_lock = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&s_eh_init_mux);
    return s_eh_lock && xSemaphoreTake(s_eh_lock, pdMS_TO_TICKS(5000)) == pdTRUE;
}
static void eh_unlock(void) { xSemaphoreGive(s_eh_lock); }

static int      s_eh_nrec;          /* slots in the area                         */
static int      s_eh_newest;        /* slot of the newest valid record, -1 none   */
static uint32_t s_eh_seq;
static int      s_eh_count;
static int64_t  s_eh_oldest;        /* slot start of the oldest record           */

/* The quarter hour being collected. */
static int64_t  s_cur;              /* its start, 0 = none                       */
static double   s_sum[4];           /* deye_bat, grid, pv, house                 */
static uint16_t s_cnt[4];
static float    s_soc_deye, s_soc_byd, s_price;
static uint16_t s_act_n[4];         /* NORMAL, HOLD, CHARGE, none                */
static uint16_t s_ticks, s_acting_n, s_paused_n;
static uint8_t  s_mode;

static uint32_t eh_rec_sum(const eh_rec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(eh_rec_t, sum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool eh_rec_ok(const eh_rec_t *r)
{
    return r->magic == EH_MAGIC && r->ver == EH_VER && r->sum == eh_rec_sum(r);
}

static bool eh_read_slot(int slot, eh_rec_t *r)
{
    return s_eh_io->read((size_t)slot * EH_REC_SIZE, r, sizeof(*r)) == ESP_OK && eh_rec_ok(r);
}

static esp_err_t em_hist_init_l(void)
{
    esp_err_t e = eh_io_open();
    if (e != ESP_OK) return e;
    s_eh_nrec = (int)(s_eh_io->size / EH_REC_SIZE);
    s_eh_newest = -1; s_eh_seq = 0; s_eh_count = 0; s_eh_oldest = 0;
    uint32_t oldest_seq = UINT32_MAX;
    /* A sector per read: 256 reads for the whole area, not 32768. */
    static EXT_RAM_BSS_ATTR eh_rec_t sec[EH_PER_SECTOR];
    for (int s0 = 0; s0 < s_eh_nrec; s0 += EH_PER_SECTOR) {
        if (s_eh_io->read((size_t)s0 * EH_REC_SIZE, sec, sizeof(sec)) != ESP_OK) continue;
        for (int k = 0; k < EH_PER_SECTOR; k++) {
            const eh_rec_t *r = &sec[k];
            if (!eh_rec_ok(r)) continue;
            s_eh_count++;
            if (s_eh_newest < 0 || r->seq > s_eh_seq) { s_eh_newest = s0 + k; s_eh_seq = r->seq; }
            if (r->seq < oldest_seq) { oldest_seq = r->seq; s_eh_oldest = r->t; }
        }
    }
    ESP_LOGI(EH_TAG, "%d Viertelstunden im Verlauf", s_eh_count);
    return ESP_OK;
}

static bool eh_blank(int slot)
{
    uint8_t b[EH_REC_SIZE];
    if (s_eh_io->read((size_t)slot * EH_REC_SIZE, b, sizeof(b)) != ESP_OK) return false;
    for (size_t i = 0; i < sizeof(b); i++) if (b[i] != 0xFF) return false;
    return true;
}

static esp_err_t eh_put_l(eh_rec_t *r)
{
    if (!s_eh_io || s_eh_nrec < EH_PER_SECTOR * 2) return ESP_ERR_INVALID_STATE;
    int  slot = s_eh_newest < 0 ? 0 : (s_eh_newest + 1) % s_eh_nrec;
    bool wrapped = false;
    /* Same rules as load_hist: a sector is wiped on entry (on the second
     * round that drops the oldest 128 quarter hours), a slot that is not
     * blank -- torn by a power loss -- is stepped over. */
    for (int guard = 0; guard < EH_PER_SECTOR; guard++) {
        if (slot % EH_PER_SECTOR == 0) {
            int lost = 0;
            eh_rec_t o;
            for (int i = slot; i < slot + EH_PER_SECTOR; i++) lost += eh_read_slot(i, &o);
            esp_err_t e = s_eh_io->erase((size_t)slot * EH_REC_SIZE, EH_SECTOR);
            if (e != ESP_OK) return e;
            s_eh_count -= lost;
            wrapped |= lost > 0;
            break;
        }
        if (eh_blank(slot)) break;
        slot = (slot + 1) % s_eh_nrec;
    }
    r->magic = EH_MAGIC; r->ver = EH_VER; r->seq = s_eh_seq + 1;
    r->sum = eh_rec_sum(r);
    esp_err_t e = s_eh_io->write((size_t)slot * EH_REC_SIZE, r, sizeof(*r));
    if (e != ESP_OK) return e;
    eh_rec_t chk;
    if (!eh_read_slot(slot, &chk) || memcmp(&chk, r, sizeof(*r)) != 0)
        return ESP_FAIL;      /* read back differs; the slot is skipped next time */
    s_eh_newest = slot; s_eh_seq = r->seq; s_eh_count++;
    if (s_eh_count == 1) s_eh_oldest = r->t;
    if (wrapped) {
        eh_rec_t o;
        for (int k = 1; k <= s_eh_nrec; k++) {
            int i = (slot + k) % s_eh_nrec;
            if (eh_read_slot(i, &o)) { s_eh_oldest = o.t; break; }
        }
    }
    return ESP_OK;
}

/* ------------------------- collecting a slot ---------------------------- */

static uint16_t enc_soc(float v)
{
    if (!isfinite(v) || v < 0.0f || v > 100.0f) return EH_U16_NONE;
    return (uint16_t)lrintf(v * 10.0f);
}
static int16_t enc_i16(double v)
{
    if (!isfinite(v)) return EH_I16_NONE;
    if (v > 32000.0) v = 32000.0;
    if (v < -32000.0) v = -32000.0;
    return (int16_t)lrint(v);
}

static void eh_reset_acc(int64_t slot)
{
    s_cur = slot;
    memset(s_sum, 0, sizeof(s_sum));
    memset(s_cnt, 0, sizeof(s_cnt));
    memset(s_act_n, 0, sizeof(s_act_n));
    s_soc_deye = s_soc_byd = s_price = NAN;
    s_ticks = s_acting_n = s_paused_n = 0;
    s_mode = 0;
}

static void eh_close_slot(void)
{
    if (!s_cur || !s_ticks) return;
    static eh_rec_t r;
    memset(&r, 0, sizeof(r));
    r.t        = (uint32_t)s_cur;
    r.deye_soc = enc_soc(s_soc_deye);
    r.byd_soc  = enc_soc(s_soc_byd);
    int16_t *w[4] = { &r.deye_bat_w, &r.grid_w, &r.pv_w, &r.house_w };
    for (int k = 0; k < 4; k++) *w[k] = s_cnt[k] ? enc_i16(s_sum[k] / s_cnt[k]) : EH_I16_NONE;
    r.price = enc_i16(isfinite(s_price) ? s_price * 100.0 : NAN);
    /* The action that was planned for most of the slot (replans move it). */
    int best = 3;
    for (int a = 0; a < 3; a++) if (s_act_n[a] > s_act_n[best]) best = a;
    r.act   = best == 3 ? EH_ACT_NONE : (uint8_t)best;
    r.mode  = s_mode;
    r.flags = (uint8_t)((s_acting_n * 2 > s_ticks ? EH_F_ACTING : 0) |
                        (s_paused_n * 2 > s_ticks ? EH_F_PAUSED : 0));
    esp_err_t e = eh_put_l(&r);
    if (e != ESP_OK) ESP_LOGW(EH_TAG, "Viertelstunde nicht gespeichert: %s", esp_err_to_name(e));
}

static void em_hist_tick_l(int64_t now, const eh_sample_t *s)
{
    int64_t slot = now - (((now % EH_SLOT_S) + EH_SLOT_S) % EH_SLOT_S);
    if (slot != s_cur) {
        eh_close_slot();
        eh_reset_acc(slot);
    }
    if (!s) return;
    s_ticks++;
    float p[4] = { s->deye_bat_w, s->grid_w, s->pv_w, s->house_w };
    for (int k = 0; k < 4; k++) if (isfinite(p[k])) { s_sum[k] += p[k]; s_cnt[k]++; }
    /* SoC: the last value of the slot, like the plan's soc_end. */
    if (isfinite(s->deye_soc)) s_soc_deye = s->deye_soc;
    if (isfinite(s->byd_soc))  s_soc_byd  = s->byd_soc;
    if (isfinite(s->price_ct)) s_price    = s->price_ct;
    s_act_n[s->act <= 2 ? s->act : 3]++;
    s_acting_n += s->acting;
    s_paused_n += s->paused;
    s_mode = s->mode;
}

/* ------------------------------- reading -------------------------------- */

static float dec_soc(uint16_t v) { return v == EH_U16_NONE ? NAN : v / 10.0f; }
static float dec_i16(int16_t v, float scale) { return v == EH_I16_NONE ? NAN : v / scale; }

static int em_hist_range_l(int64_t t0, int n, eh_slot_t *out)
{
    t0 -= ((t0 % EH_SLOT_S) + EH_SLOT_S) % EH_SLOT_S;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].act = EH_ACT_NONE;
        out[i].deye_soc = out[i].byd_soc = out[i].deye_bat_w = out[i].grid_w = NAN;
        out[i].pv_w = out[i].house_w = out[i].price_ct = NAN;
    }
    if (!s_eh_io || s_eh_newest < 0 || n <= 0) return 0;
    /* Newest first; the first record seen for a slot wins (a slot written
     * twice after a clock jump keeps the later one). Bounded: twice the
     * range plus a sector, and nothing older than a day before t0. */
    int found = 0, limit = 2 * n + EH_PER_SECTOR;
    eh_rec_t r;
    for (int back = 0; back < s_eh_nrec && back < limit; back++) {
        int i = ((s_eh_newest - back) % s_eh_nrec + s_eh_nrec) % s_eh_nrec;
        if (!eh_read_slot(i, &r)) continue;
        if ((int64_t)r.t < t0 - EH_LOOKBACK_S) break;
        if ((int64_t)r.t < t0) continue;
        int64_t k = ((int64_t)r.t - t0) / EH_SLOT_S;
        if (k >= n || out[k].valid) continue;
        eh_slot_t *o = &out[k];
        o->valid      = true;
        o->act        = r.act;
        o->mode       = r.mode;
        o->flags      = r.flags;
        o->deye_soc   = dec_soc(r.deye_soc);
        o->byd_soc    = dec_soc(r.byd_soc);
        o->deye_bat_w = dec_i16(r.deye_bat_w, 1.0f);
        o->grid_w     = dec_i16(r.grid_w, 1.0f);
        o->pv_w       = dec_i16(r.pv_w, 1.0f);
        o->house_w    = dec_i16(r.house_w, 1.0f);
        o->price_ct   = dec_i16(r.price, 100.0f);
        found++;
    }
    return found;
}

/* -------------------------------- public -------------------------------- */

esp_err_t em_hist_init(void)
{
    if (!eh_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t r = em_hist_init_l();
    eh_unlock();
    return r;
}

void em_hist_tick(int64_t now, const eh_sample_t *s)
{
    if (!eh_lock()) return;
    em_hist_tick_l(now, s);
    eh_unlock();
}

int em_hist_range(int64_t t0, int n, eh_slot_t *out)
{
    if (!out || n <= 0) return 0;
    if (!eh_lock()) {
        for (int i = 0; i < n; i++) { memset(&out[i], 0, sizeof(out[i])); out[i].act = EH_ACT_NONE; }
        return 0;
    }
    int r = em_hist_range_l(t0, n, out);
    eh_unlock();
    return r;
}

int em_hist_count(int64_t *oldest)
{
    if (!eh_lock()) { if (oldest) *oldest = 0; return 0; }
    int r = s_eh_count;
    if (oldest) *oldest = r ? s_eh_oldest : 0;
    eh_unlock();
    return r;
}

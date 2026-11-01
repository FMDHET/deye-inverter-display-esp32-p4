#include "load_hist.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR: buffers in PSRAM, internal RAM is for WiFi/TLS */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *LH_TAG = "load_hist";

#define LH_REC_SIZE   256
#define LH_SECTOR     4096
#define LH_PER_SECTOR (LH_SECTOR / LH_REC_SIZE)
#define LH_MAGIC      0x4C484431u          /* "LHD1" */
#define LH_LABEL      "loadhist"
#define LH_SUBTYPE    0x40
#define LH_OFFSET     0xA00000
#define LH_SIZE       0x100000
#define LH_TRIM       0.10f                /* share dropped at each end      */
#define LH_SCAN_MAX   400                  /* records looked at for an average */

/* One record in flash. The checksum covers everything before it; a record
 * cut short by a power loss mid-write fails it and is skipped. */
typedef struct {
    uint32_t magic;
    uint32_t seq;                /* write order, the newest has the highest  */
    int32_t  day;
    uint8_t  wday, ver;
    uint16_t nslots;
    uint16_t w[LH_SLOTS];
    uint32_t sum;                /* FNV-1a over all of the above             */
} lh_rec_t;
_Static_assert(sizeof(lh_rec_t) <= LH_REC_SIZE, "record fits its slot");

/* ------------------------------ I/O hooks -------------------------------
 * Flash is the only part that cannot run on the host: read, write and erase
 * of the area sit behind three pointers, the host suite puts RAM there. */
typedef struct {
    esp_err_t (*read)(size_t off, void *buf, size_t len);
    esp_err_t (*write)(size_t off, const void *buf, size_t len);
    esp_err_t (*erase)(size_t off, size_t len);
    size_t    size;
} lh_io_t;

#ifndef LOAD_HIST_HOST_TEST
#include "esp_flash.h"
#include "esp_partition.h"

static const esp_partition_t *s_lh_part;
static esp_err_t lh_p_read(size_t o, void *b, size_t l)        { return esp_partition_read(s_lh_part, o, b, l); }
static esp_err_t lh_p_write(size_t o, const void *b, size_t l) { return esp_partition_write(s_lh_part, o, b, l); }
static esp_err_t lh_p_erase(size_t o, size_t l)                { return esp_partition_erase_range(s_lh_part, o, l); }
static lh_io_t s_lh_part_io = { lh_p_read, lh_p_write, lh_p_erase, 0 };
static lh_io_t *s_lh_io;

static esp_err_t lh_io_open(void)
{
    if (s_lh_io) return ESP_OK;
    s_lh_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, LH_SUBTYPE, LH_LABEL);
    if (!s_lh_part) {
        /* Not in the table on flash (an OTA-only device): register the same
         * area at runtime. It refuses an overlap with any known partition. */
        esp_err_t e = esp_partition_register_external(esp_flash_default_chip, LH_OFFSET, LH_SIZE,
                                                      LH_LABEL, ESP_PARTITION_TYPE_DATA, LH_SUBTYPE, &s_lh_part);
        if (e != ESP_OK) {
            ESP_LOGE(LH_TAG, "Flashbereich 0x%x nicht verfügbar: %s", LH_OFFSET, esp_err_to_name(e));
            return e;
        }
        ESP_LOGI(LH_TAG, "Flashbereich 0x%x (%u kB) zur Laufzeit eingebunden", LH_OFFSET, LH_SIZE / 1024);
    }
    s_lh_part_io.size = s_lh_part->size;
    s_lh_io = &s_lh_part_io;
    return ESP_OK;
}
#else
static lh_io_t *s_lh_io;                     /* the host suite installs its fake */
static esp_err_t lh_io_open(void) { return s_lh_io ? ESP_OK : ESP_ERR_NOT_FOUND; }
#endif

/* ------------------------------- state ---------------------------------- */

/* The energy manager writes (midnight), the web server reads (the daily
 * curves): one lock around every public call -- the record buffers below
 * are static. */
static SemaphoreHandle_t s_lh_lock;
static portMUX_TYPE      s_lh_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool lh_lock(void)
{
    portENTER_CRITICAL(&s_lh_init_mux);
    if (!s_lh_lock) s_lh_lock = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&s_lh_init_mux);
    return s_lh_lock && xSemaphoreTake(s_lh_lock, pdMS_TO_TICKS(5000)) == pdTRUE;
}
static void lh_unlock(void) { xSemaphoreGive(s_lh_lock); }

static int      s_lh_nrec;          /* slots in the area                         */
static int      s_lh_newest;        /* slot of the newest valid record, -1 none   */
static uint32_t s_lh_seq;           /* its sequence number                        */
static int      s_lh_count;         /* valid records                              */
static int32_t  s_lh_oldest;

static uint32_t lh_rec_sum(const lh_rec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(lh_rec_t, sum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool lh_rec_ok(const lh_rec_t *r)
{
    return r->magic == LH_MAGIC && r->ver == 1 && r->nslots == LH_SLOTS && r->sum == lh_rec_sum(r);
}

static bool lh_read_slot(int slot, lh_rec_t *r)
{
    return s_lh_io->read((size_t)slot * LH_REC_SIZE, r, sizeof(*r)) == ESP_OK && lh_rec_ok(r);
}

static esp_err_t load_hist_init_l(void)
{
    esp_err_t e = lh_io_open();
    if (e != ESP_OK) return e;
    s_lh_nrec = (int)(s_lh_io->size / LH_REC_SIZE);
    s_lh_newest = -1; s_lh_seq = 0; s_lh_count = 0; s_lh_oldest = 0;
    uint32_t oldest_seq = UINT32_MAX;
    static EXT_RAM_BSS_ATTR lh_rec_t r;
    for (int i = 0; i < s_lh_nrec; i++) {
        if (!lh_read_slot(i, &r)) continue;
        s_lh_count++;
        if (s_lh_newest < 0 || r.seq > s_lh_seq) { s_lh_newest = i; s_lh_seq = r.seq; }
        if (r.seq < oldest_seq) { oldest_seq = r.seq; s_lh_oldest = r.day; }
    }
    ESP_LOGI(LH_TAG, "%d Tage im Langzeitspeicher", s_lh_count);
    return ESP_OK;
}

static bool lh_blank(int slot)
{
    static EXT_RAM_BSS_ATTR uint8_t b[LH_REC_SIZE];
    if (s_lh_io->read((size_t)slot * LH_REC_SIZE, b, sizeof(b)) != ESP_OK) return false;
    for (size_t i = 0; i < sizeof(b); i++) if (b[i] != 0xFF) return false;
    return true;
}

static esp_err_t load_hist_put_l(const lh_day_t *d)
{
    if (!s_lh_io || s_lh_nrec < LH_PER_SECTOR * 2) return ESP_ERR_INVALID_STATE;
    int  slot = s_lh_newest < 0 ? 0 : (s_lh_newest + 1) % s_lh_nrec;
    bool wrapped = false;
    /* A slot that is not blank cannot take a record without an erase: a
     * record torn by a power loss right after the newest one. Step over it
     * (it fails its checksum and stays unread). */
    for (int guard = 0; guard < LH_PER_SECTOR; guard++) {
        if (slot % LH_PER_SECTOR == 0) {
            /* Entering a sector: wipe it first. On the ring's second round
             * that drops the 16 oldest days -- after ~11 years. */
            int lost = 0;
            static EXT_RAM_BSS_ATTR lh_rec_t o;
            for (int i = slot; i < slot + LH_PER_SECTOR; i++) lost += lh_read_slot(i, &o);
            esp_err_t e = s_lh_io->erase((size_t)slot * LH_REC_SIZE, LH_SECTOR);
            if (e != ESP_OK) return e;
            s_lh_count -= lost;
            wrapped |= lost > 0;
            break;
        }
        if (lh_blank(slot)) break;
        slot = (slot + 1) % s_lh_nrec;
    }
    static EXT_RAM_BSS_ATTR lh_rec_t r;
    memset(&r, 0xFF, sizeof(r));
    r.magic = LH_MAGIC; r.seq = s_lh_seq + 1; r.day = d->day; r.wday = d->wday;
    r.ver = 1; r.nslots = LH_SLOTS;
    memcpy(r.w, d->w, sizeof(r.w));
    r.sum = lh_rec_sum(&r);
    esp_err_t e = s_lh_io->write((size_t)slot * LH_REC_SIZE, &r, sizeof(r));
    if (e != ESP_OK) return e;
    static EXT_RAM_BSS_ATTR lh_rec_t chk;
    if (!lh_read_slot(slot, &chk) || memcmp(&chk, &r, sizeof(r)) != 0)
        return ESP_FAIL;      /* read back differs; the slot is skipped next time */
    s_lh_newest = slot; s_lh_seq = r.seq; s_lh_count++;
    if (s_lh_count == 1) s_lh_oldest = d->day;
    /* After a wrap dropped days the oldest is wherever the ring continues. */
    if (wrapped) {
        static EXT_RAM_BSS_ATTR lh_rec_t o;
        for (int k = 1; k <= s_lh_nrec; k++) {
            int i = (slot + k) % s_lh_nrec;
            if (lh_read_slot(i, &o)) { s_lh_oldest = o.day; break; }
        }
    }
    return ESP_OK;
}

static int load_hist_count_l(int32_t *oldest_day)
{
    if (oldest_day) *oldest_day = s_lh_count ? s_lh_oldest : 0;
    return s_lh_count;
}

static bool load_hist_get_l(int k, lh_day_t *out)
{
    if (!s_lh_io || s_lh_newest < 0 || k < 0) return false;
    static EXT_RAM_BSS_ATTR lh_rec_t r;
    int seen = 0;
    for (int back = 0; back < s_lh_nrec; back++) {
        int i = ((s_lh_newest - back) % s_lh_nrec + s_lh_nrec) % s_lh_nrec;
        if (!lh_read_slot(i, &r)) continue;
        if (seen++ == k) {
            out->day = r.day; out->wday = r.wday;
            memcpy(out->w, r.w, sizeof(out->w));
            return true;
        }
    }
    return false;
}

static int lh_cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static void load_hist_average_l(uint8_t we, int max_days, float out_w[LH_SLOTS], uint16_t *ndays)
{
    for (int i = 0; i < LH_SLOTS; i++) out_w[i] = NAN;
    if (ndays) *ndays = 0;
    if (!s_lh_io || s_lh_newest < 0 || max_days <= 0) return;
    if (max_days > LH_AVG_WD) max_days = LH_AVG_WD;

    /* Newest first; a date seen already (a day written twice) is skipped. */
    static EXT_RAM_BSS_ATTR uint16_t v[LH_SLOTS][LH_AVG_WD];
    static EXT_RAM_BSS_ATTR int32_t  days[LH_AVG_WD];
    static EXT_RAM_BSS_ATTR lh_rec_t r;
    int n = 0;
    for (int back = 0; back < s_lh_nrec && back < LH_SCAN_MAX && n < max_days; back++) {
        int i = ((s_lh_newest - back) % s_lh_nrec + s_lh_nrec) % s_lh_nrec;
        if (!lh_read_slot(i, &r)) continue;
        if ((uint8_t)(r.wday == 0 || r.wday == 6) != we) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= days[k] == r.day;
        if (dup) continue;
        days[n] = r.day;
        for (int s = 0; s < LH_SLOTS; s++) v[s][n] = r.w[s];
        n++;
    }
    if (ndays) *ndays = (uint16_t)n;
    /* Trimmed mean per slot over the days that measured it. */
    uint16_t col[LH_AVG_WD];
    for (int s = 0; s < LH_SLOTS; s++) {
        int m = 0;
        for (int k = 0; k < n; k++) if (v[s][k] != LH_NONE) col[m++] = v[s][k];
        if (!m) continue;
        qsort(col, (size_t)m, sizeof(col[0]), lh_cmp_u16);
        int cut = m >= 5 ? (int)(m * LH_TRIM + 0.5f) : 0;
        double sum = 0.0;
        for (int k = cut; k < m - cut; k++) sum += col[k];
        out_w[s] = (float)(sum / (m - 2 * cut));
    }
}

void load_hist_date(int32_t day, int *y, int *m, int *d)
{
    /* civil_from_days (H. Hinnant), day 0 = 1970-01-01 */
    int32_t z = day + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

esp_err_t load_hist_init(void)
{
    if (!lh_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t r = load_hist_init_l();
    lh_unlock();
    return r;
}

esp_err_t load_hist_put(const lh_day_t *d)
{
    if (!lh_lock()) return ESP_ERR_TIMEOUT;
    esp_err_t r = load_hist_put_l(d);
    lh_unlock();
    return r;
}

int load_hist_count(int32_t *oldest_day)
{
    if (!lh_lock()) return 0;
    int r = load_hist_count_l(oldest_day);
    lh_unlock();
    return r;
}

bool load_hist_get(int k, lh_day_t *out)
{
    if (!lh_lock()) return false;
    bool r = load_hist_get_l(k, out);
    lh_unlock();
    return r;
}

void load_hist_average(uint8_t we, int max_days, float out_w[LH_SLOTS], uint16_t *ndays)
{
    if (!lh_lock()) {
        for (int i = 0; i < LH_SLOTS; i++) out_w[i] = NAN;
        if (ndays) *ndays = 0;
        return;
    }
    load_hist_average_l(we, max_days, out_w, ndays);
    lh_unlock();
}

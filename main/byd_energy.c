#include "byd_energy.h"
#include "energy_ctr.h"
#include "nvs_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "byd_e";

/* NVS record. Fields are only ever appended. */
typedef struct {
    double chg_wh, dis_wh;
} byd_e_rec_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static byd_e_rec_t  s_sum;        /* running sums                          */
static byd_e_rec_t  s_saved;      /* what NVS holds                        */
static bool         s_have;       /* stored or sampled at least once       */
static int          s_slot = -1;  /* the device slot being counted         */
static uint32_t     s_last_ms;    /* previous sample of that slot (0 = none) */
static uint32_t     s_saved_ms;   /* last NVS write                        */

static void save(bool force, uint32_t now_ms)
{
    byd_e_rec_t r;
    portENTER_CRITICAL(&s_mux);
    r = s_sum;
    portEXIT_CRITICAL(&s_mux);
    if (memcmp(&r, &s_saved, sizeof(r)) == 0) return;
    if (!force && (uint32_t)(now_ms - s_saved_ms) < BYD_ENERGY_SAVE_MS) return;
    if (nvs_store_set_byd_energy(&r, sizeof(r)) == ESP_OK) {
        s_saved    = r;
        s_saved_ms = now_ms;
    }
}

static void shutdown_save(void) { save(true, 0); }

void byd_energy_init(void)
{
    byd_e_rec_t r;
    memset(&r, 0, sizeof(r));
    if (nvs_store_get_byd_energy(&r, sizeof(r)) == ESP_OK &&
        r.chg_wh >= 0 && r.dis_wh >= 0) {
        portENTER_CRITICAL(&s_mux);
        s_sum = r; s_have = true;
        portEXIT_CRITICAL(&s_mux);
        s_saved = r;
        ESP_LOGI(TAG, "loaded: charged %.2f kWh, discharged %.2f kWh",
                 r.chg_wh / 1000.0, r.dis_wh / 1000.0);
    }
    esp_register_shutdown_handler(shutdown_save);
}

void byd_energy_sample(int slot, float w, uint32_t now_ms, uint32_t max_gap_ms)
{
    portENTER_CRITICAL(&s_mux);
    /* Take over from a slot that has gone quiet (reconfigured, removed). */
    if (s_slot != slot && s_slot >= 0 && s_last_ms &&
        (uint32_t)(now_ms - s_last_ms) <= max_gap_ms) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (s_slot != slot) { s_slot = slot; s_last_ms = 0; }
    uint32_t dt = (uint32_t)(now_ms - s_last_ms);
    if (s_last_ms && dt <= max_gap_ms)
        energy_integrate(&s_sum.chg_wh, &s_sum.dis_wh, w, dt);
    s_last_ms = now_ms ? now_ms : 1;
    s_have = true;
    portEXIT_CRITICAL(&s_mux);

    save(false, now_ms);
}

bool byd_energy_get(double *chg_kwh, double *dis_kwh)
{
    portENTER_CRITICAL(&s_mux);
    bool have = s_have;
    byd_e_rec_t r = s_sum;
    portEXIT_CRITICAL(&s_mux);
    if (chg_kwh) *chg_kwh = r.chg_wh / 1000.0;
    if (dis_kwh) *dis_kwh = r.dis_wh / 1000.0;
    return have;
}

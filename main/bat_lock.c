#include "bat_lock.h"
#include "fronius_storage.h"
#include "modbus_rtu.h"
#include "nvs_store.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bat_lock";

#define BL_CLOCK_OK  1700000000LL        /* earlier = clock not set yet */
#define BL_VER       2

/* NVS "em/dlock". Version 1 (build 358) held the Deye lock alone, in the
 * same place as `deye` here: read on, so an existing lock survives the
 * update. */
typedef struct {
    uint16_t ver;
    uint8_t  on, _rsv;
    int64_t  until;
} bl_rec_v1_t;
typedef struct {
    uint16_t ver;
    uint8_t  on[BAT_N], _rsv[2];
    int64_t  until[BAT_N];
} bl_rec_t;

static const char *const NAME[BAT_N] = { "Deye", "BYD" };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bat_lock_t   s_lock[BAT_N];       /* under s_mux */

static void persist(void)
{
    bl_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ver = BL_VER;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < BAT_N; i++) { r.on[i] = s_lock[i].on; r.until[i] = s_lock[i].until; }
    portEXIT_CRITICAL(&s_mux);
    if (nvs_store_set_dlock(&r, sizeof(r)) != ESP_OK)
        ESP_LOGW(TAG, "Zustand nicht gespeichert -- ein Neustart hebt die Sperre auf");
}

void bat_lock_init(void)
{
    bl_rec_t r;
    memset(&r, 0, sizeof(r));
    bat_lock_t l[BAT_N];
    memset(l, 0, sizeof(l));
    if (nvs_store_get_dlock(&r, sizeof(r)) == ESP_OK) {
        if (r.ver == BL_VER) {
            for (int i = 0; i < BAT_N; i++) { l[i].on = r.on[i]; l[i].until = r.on[i] ? r.until[i] : 0; }
        } else if (r.ver == 1) {
            const bl_rec_v1_t *o = (const bl_rec_v1_t *)&r;
            l[BAT_DEYE].on = o->on;
            l[BAT_DEYE].until = o->on ? o->until : 0;
        }
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_lock, l, sizeof(s_lock));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < BAT_N; i++)
        if (l[i].on) ESP_LOGW(TAG, "%s-Akku bleibt gesperrt (%s)", NAME[i], l[i].until ? "befristet" : "bis aufgehoben");
    if (l[BAT_DEYE].on) modbus_rtu_set_lock(BAT_LOCK_RENEW_S);
}

esp_err_t bat_lock_set(bat_t which, bool on, uint32_t minutes, int64_t now)
{
    if ((unsigned)which >= BAT_N) return ESP_ERR_INVALID_ARG;
    bat_lock_t l = { 0 };
    if (on) {
        if (minutes > BAT_LOCK_MAX_MIN) return ESP_ERR_INVALID_ARG;
        if (minutes && now < BL_CLOCK_OK) return ESP_ERR_INVALID_STATE;
        l.on    = true;
        l.until = minutes ? now + (int64_t)minutes * 60 : 0;
    }
    portENTER_CRITICAL(&s_mux);
    s_lock[which] = l;
    portEXIT_CRITICAL(&s_mux);
    persist();
    if (which == BAT_DEYE) modbus_rtu_set_lock(on ? BAT_LOCK_RENEW_S : 0);
    if (which == BAT_BYD && !on) fronius_storage_lock_discharge(false);   /* lifted at once */
    ESP_LOGW(TAG, "%s-Akku %s", NAME[which],
             !on ? "freigegeben" : minutes ? "gesperrt (befristet)" : "gesperrt bis aufgehoben");
    return ESP_OK;
}

void bat_lock_tick(int64_t now, bool byd_allowed)
{
    bat_lock_t l[BAT_N];
    portENTER_CRITICAL(&s_mux);
    memcpy(l, s_lock, sizeof(l));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < BAT_N; i++) {
        /* A timed lock ends by the clock; while the clock is not set (right
         * after a restart) it stays on rather than guessing. */
        if (l[i].on && l[i].until && now >= BL_CLOCK_OK && now >= l[i].until) {
            bat_lock_set((bat_t)i, false, 0, now);
            ESP_LOGW(TAG, "%s: Sperrzeit abgelaufen", NAME[i]);
            l[i].on = false;
        }
    }
    if (l[BAT_DEYE].on) modbus_rtu_set_lock(BAT_LOCK_RENEW_S);
    fronius_storage_lock_discharge(l[BAT_BYD].on && byd_allowed);
}

void bat_lock_get(bat_t which, bat_lock_t *out)
{
    if (!out) return;
    if ((unsigned)which >= BAT_N) { memset(out, 0, sizeof(*out)); return; }
    portENTER_CRITICAL(&s_mux);
    *out = s_lock[which];
    portEXIT_CRITICAL(&s_mux);
}

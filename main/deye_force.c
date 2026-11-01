#include "deye_force.h"
#include "deye_ctrl.h"
#include "modbus_rtu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "deye_force";

#define DF_CLOCK_OK  1700000000LL

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static deye_force_t s_f;                 /* under s_mux */

static deye_mode_t ctrl_mode(uint8_t m)
{
    return m == DEYE_FORCE_CHARGE ? DEYE_MODE_FORCE_CHARGE : DEYE_MODE_FORCE_DISCHARGE;
}

/* End the session; back to Normal only if the Deye still runs OUR mode --
 * a takeover by someone else is left alone. */
static void finish(const deye_force_t *seen, const char *why, bool to_normal)
{
    if (to_normal && deye_ctrl_get_mode() == ctrl_mode(seen->mode))
        deye_ctrl_apply(DEYE_MODE_NORMAL, deye_ctrl_get_user_power());
    portENTER_CRITICAL(&s_mux);
    if (s_f.mode == seen->mode && s_f.until == seen->until) {
        s_f.mode = DEYE_FORCE_OFF;
        s_f.active = false;
        snprintf(s_f.end, sizeof(s_f.end), "%s", why);
    }
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "Zwangsbetrieb Deye beendet: %s", why);
}

esp_err_t deye_force_set(deye_force_mode_t mode, uint16_t w, uint16_t minutes, uint8_t soc, int64_t now)
{
    if (mode == DEYE_FORCE_OFF) {
        portENTER_CRITICAL(&s_mux);
        deye_force_t f = s_f;
        portEXIT_CRITICAL(&s_mux);
        if (f.mode) finish(&f, "von Hand beendet", true);
        return ESP_OK;
    }
    if (mode != DEYE_FORCE_CHARGE && mode != DEYE_FORCE_DISCHARGE) return ESP_ERR_INVALID_ARG;
    if (w < DEYE_FORCE_MIN_W || w > DEYE_POWER_MAX) return ESP_ERR_INVALID_ARG;
    if (minutes == 0) minutes = 60;
    if (minutes > DEYE_FORCE_MAX_MIN) return ESP_ERR_INVALID_ARG;
    if (soc < 10 || soc > 100) return ESP_ERR_INVALID_ARG;
    if (now < DF_CLOCK_OK) return ESP_ERR_INVALID_STATE;
    /* The charge target goes into the inverter; for a discharge the TOU
     * SoC is not a floor, so 100 there and the floor watched here. */
    esp_err_t e = deye_ctrl_apply_auto(ctrl_mode(mode), w, mode == DEYE_FORCE_CHARGE ? soc : 100,
                                       (uint32_t)minutes * 60u);
    if (e != ESP_OK) return e;
    portENTER_CRITICAL(&s_mux);
    s_f.mode = (uint8_t)mode; s_f.w = w; s_f.until = now + (int64_t)minutes * 60;
    s_f.soc = soc; s_f.active = true; s_f.end[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "Deye %s %u W, %u min, bis %u %%", mode == DEYE_FORCE_CHARGE ? "Zwangsladen" : "Zwangsentladen",
             w, minutes, soc);
    return ESP_OK;
}

void deye_force_tick(int64_t now)
{
    portENTER_CRITICAL(&s_mux);
    deye_force_t f = s_f;
    portEXIT_CRITICAL(&s_mux);
    if (!f.mode) return;

    /* Someone else changed the Deye (display, MQTT, sweep, deye_ctrl's own
     * fallback at the end of the time): the session is over, nothing to undo. */
    bool ours = deye_ctrl_get_mode() == ctrl_mode(f.mode) && deye_ctrl_get_user_power() == (int)f.w;
    if (!ours) {
        bool timeout = now >= DF_CLOCK_OK && now >= f.until - 15;   /* deye_ctrl's fallback came first */
        finish(&f, timeout ? "Zeit abgelaufen" : "am Display oder per MQTT geändert", false);
        return;
    }
    if (now >= DF_CLOCK_OK && now >= f.until) { finish(&f, "Zeit abgelaufen", true); return; }

    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    if (l.valid && l.online && isfinite(l.bat_soc)) {
        if ((f.mode == DEYE_FORCE_CHARGE && l.bat_soc >= f.soc) ||
            (f.mode == DEYE_FORCE_DISCHARGE && l.bat_soc <= f.soc)) {
            finish(&f, "Ziel-SoC erreicht", true);
            return;
        }
    }
}

void deye_force_get(deye_force_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_f;
    portEXIT_CRITICAL(&s_mux);
}

#include "deye_ctrl.h"
#include "modbus_rtu.h"
#include "nvs_store.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "deye_ctrl";

/* Deye SG04LP3 control registers (write via FC16 over the RTU master bus). */
#define REG_WORK_MODE   142     /* 2 = Zero Export to CT, 3 = Selling First */
#define REG_SELL_POWER  143     /* Max Sell Power (W)                       */
#define REG_NORMAL_126  126     /* additional Normal-mode reset registers    */
#define REG_NORMAL_127  127
#define REG_NORMAL_128  128
/* DEYE_POWER_MIN/MAX live in deye_ctrl.h (shared with the UI and MQTT). */

/* Last requested mode + power magnitude (W).
 * s_user_power_w = what the user set (slider/MQTT); never changed by the guard.
 * s_power_w      = what is actually applied (may be throttled by SLS guard). */
static volatile deye_mode_t s_mode         = DEYE_MODE_NORMAL;
static volatile int         s_power_w      = 5000;
static volatile int         s_user_power_w = 5000;
static TaskHandle_t         s_task;

/* When the current mode was applied (uptime ms) -- drives the automatic
 * fallback after DEYE_FORCE_MAX_S. */
static volatile uint32_t    s_mode_ms;
/* Verification result of the last apply (see write_verify). */
static volatile uint8_t     s_checked, s_failed;
/* Set at start when a forced mode survived a restart in the INVERTER and has to
 * be undone; s_undo_mode only names it in the log. */
static volatile bool        s_undo_pending;
static volatile uint8_t     s_undo_mode, s_undo_tries;
#define DEYE_UNDO_TRIES     6            /* ~1 min of 10-s ticks */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Write a register and read it back. The twelve writes of a mode change used to
 * discard every return code, so a mode could fail silently and the display
 * still claimed it was in force. Read-back is done for the DECISIVE registers
 * only: at 9600 baud each transaction costs the Deye poll a gap, and the
 * time-of-use arrays (166-177) are 12 more registers whose failure the write
 * return code already reveals. */
static int write_verify(uint16_t reg, uint16_t val)
{
    int rc = modbus_rtu_deye_write(reg, val);
    if (rc != 0) {
        s_checked++; s_failed++;
        ESP_LOGE(TAG, "reg%u = %u: write failed (rc=%d)", reg, val, rc);
        return rc;
    }
    uint16_t back = 0;
    int rr = modbus_rtu_deye_read(reg, 1, &back);
    s_checked++;
    if (rr != 0) {
        s_failed++;
        ESP_LOGE(TAG, "reg%u = %u: written, but read-back failed (rc=%d)", reg, val, rr);
        return rr;
    }
    if (back != val) {
        s_failed++;
        ESP_LOGE(TAG, "reg%u = %u: inverter reports %u -- NOT applied", reg, val, back);
        return -1;
    }
    ESP_LOGI(TAG, "reg%u = %u verified", reg, val);
    return 0;
}

const char *deye_ctrl_mode_name(deye_mode_t m)
{
    switch (m) {
    case DEYE_MODE_FORCE_CHARGE:    return "Forced-Charge";
    case DEYE_MODE_FORCE_DISCHARGE: return "Forced-Discharge";
    case DEYE_MODE_NORMAL:
    default:                        return "Normal";
    }
}

deye_mode_t deye_ctrl_get_mode(void)       { return s_mode; }
int         deye_ctrl_get_power(void)      { return s_power_w; }
int         deye_ctrl_get_user_power(void) { return s_user_power_w; }

/* Perform the Modbus writes for a mode. Runs on the deye_ctrl task (NOT the LVGL
 * task) because modbus_rtu_deye_write() blocks until the RTU master bus serves
 * the request -- doing that on the UI task would freeze the touch display. */
static void deye_ctrl_write_regs(deye_mode_t mode, int power_w)
{
    int rc1 = 0, rc2 = 0;
    s_checked = s_failed = 0;
    switch (mode) {
    case DEYE_MODE_FORCE_CHARGE: {            /* charge: reg128 = charge current (A) @ ~50V */
        int rc3, rc4;
        int amps = power_w / 50;             /* WR ignores reg126(W); reacts to reg128 in A */
        rc1 = modbus_rtu_deye_write(REG_NORMAL_126, (uint16_t)power_w);
        rc3 = write_verify(REG_NORMAL_127, 99);
        rc4 = write_verify(REG_NORMAL_128, (uint16_t)amps);
        /* SOC set-points for the six time-of-use slots all to 99 % */
        for (int reg = 166; reg <= 171; reg++)
            modbus_rtu_deye_write(reg, 99);
        /* grid-charge enable flags for the six slots all ON */
        for (int reg = 172; reg <= 177; reg++)
            modbus_rtu_deye_write(reg, 1);
        ESP_LOGW(TAG, "Laden -> reg126=%d (rc=%d) reg127=99 (rc=%d) reg128=%dA (rc=%d) reg166-171=99 reg172-177=1 [%u/%u verified]",
                 power_w, rc1, rc3, amps, rc4,
                 (unsigned)(s_checked - s_failed), (unsigned)s_checked);
        return;
    }
    case DEYE_MODE_FORCE_DISCHARGE:          /* discharge: Selling First @ power */
        rc1 = write_verify(REG_WORK_MODE,  3);
        rc2 = write_verify(REG_SELL_POWER, (uint16_t)power_w);
        break;
    case DEYE_MODE_NORMAL: {              /* back to Zero-Export-to-CT, full sell */
        int rc3, rc4, rc5;
        rc1 = write_verify(REG_WORK_MODE,  2);
        rc2 = write_verify(REG_SELL_POWER, 20000);
        rc3 = modbus_rtu_deye_write(REG_NORMAL_126, 5000);
        rc4 = modbus_rtu_deye_write(REG_NORMAL_127, 10);
        rc5 = modbus_rtu_deye_write(REG_NORMAL_128, 40);
        /* SOC set-points for the six time-of-use slots all back to 13 % */
        for (int reg = 166; reg <= 171; reg++)
            modbus_rtu_deye_write(reg, 13);
        /* grid-charge enable flags for the six slots all OFF */
        for (int reg = 172; reg <= 177; reg++)
            modbus_rtu_deye_write(reg, 0);
        ESP_LOGW(TAG, "Normal -> reg142=%d reg143=%d reg126=%d reg127=%d reg128=%d reg166-171=13 reg172-177=0 [%u/%u verified]",
                 rc1, rc2, rc3, rc4, rc5,
                 (unsigned)(s_checked - s_failed), (unsigned)s_checked);
        return;
    }
    default:
        return;
    }
    ESP_LOGW(TAG, "%s -> reg142 rc=%d, reg143 rc=%d (power=%d W) [%u/%u verified]",
             deye_ctrl_mode_name(mode), rc1, rc2, power_w,
             (unsigned)(s_checked - s_failed), (unsigned)s_checked);
}

/* Put the inverter back to Normal and record that as the standing state.
 * Returns true when the decisive registers came back confirmed. */
static bool fall_back_to_normal(const char *why)
{
    ESP_LOGW(TAG, "%s -> writing Normal to the inverter", why);
    s_mode    = DEYE_MODE_NORMAL;
    s_power_w = s_user_power_w;
    s_mode_ms = now_ms();
    deye_ctrl_write_regs(DEYE_MODE_NORMAL, s_power_w);

    /* Forget the stored mode only once the inverter has CONFIRMED Normal.
     * Clearing it first would lose the one piece of knowledge that matters --
     * that the inverter is still forced -- the moment the write fails (bus not
     * up yet, Deye unreachable). Kept, the next start tries again. */
    if (s_failed == 0) {
        nvs_store_set_deye_mode((uint8_t)DEYE_MODE_NORMAL);
        return true;
    }
    ESP_LOGE(TAG, "Normal not confirmed (%u of %u registers) -- keeping the stored "
                  "mode so it is retried", (unsigned)s_failed, (unsigned)s_checked);
    return false;
}

static void deye_ctrl_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wake on an apply request, but at least every 10 s: the time limit and
         * the restart undo must happen without anyone pressing anything. */
        uint32_t requested = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

        if (s_undo_pending) {
            /* A forced mode lives in the INVERTER, not in us: it survived our
             * restart while our own state came back as "Normal". Undo it rather
             * than resume it -- a restart (OTA, crash, power cut) must not leave
             * the battery charging from the grid behind a display that says
             * Normal. The stored mode was read in deye_ctrl_start().
             * Retried on the 10-s tick while the write does not confirm (the
             * RS485 master may still be coming up), but not forever: after
             * DEYE_UNDO_TRIES the stored mode stays and the next start is the
             * next chance, instead of writing to a dead bus every 10 s. */
            char msg[96];
            snprintf(msg, sizeof(msg), "restart with '%s' still set in the inverter (try %u/%u)",
                     deye_ctrl_mode_name((deye_mode_t)s_undo_mode),
                     (unsigned)(s_undo_tries + 1), (unsigned)DEYE_UNDO_TRIES);
            s_undo_tries++;
            if (fall_back_to_normal(msg) || s_undo_tries >= DEYE_UNDO_TRIES) {
                if (s_failed != 0)
                    ESP_LOGE(TAG, "giving up on undoing '%s' for now -- the inverter "
                                  "may still be in it, check the RS485 master bus",
                             deye_ctrl_mode_name((deye_mode_t)s_undo_mode));
                s_undo_pending = false;
            }
            continue;
        }

        if (s_mode != DEYE_MODE_NORMAL &&
            (uint32_t)(now_ms() - s_mode_ms) > (uint32_t)DEYE_FORCE_MAX_S * 1000u) {
            char msg[80];
            snprintf(msg, sizeof(msg), "'%s' active for more than %d min",
                     deye_ctrl_mode_name(s_mode), DEYE_FORCE_MAX_S / 60);
            fall_back_to_normal(msg);
            continue;
        }

        if (requested) deye_ctrl_write_regs(s_mode, s_power_w);  /* latest wins */
    }
}

void deye_ctrl_start(void)
{
    /* core 0: keep the UI core (1) free; this task is mostly blocked on the RTU
     * bus anyway. Call AFTER modbus_rtu_start() (needs its request mutex). */
    if (s_task) return;

    /* Was a forced mode in force when we last ran? Then the inverter is still
     * in it. Remember to undo it -- deliberately NOT notifying the task, so the
     * first timeout wake does it ~10 s from now, by which time the RTU master
     * has the bus up and is polling. */
    uint8_t stored = nvs_store_get_deye_mode();
    uint16_t stored_pw = nvs_store_get_deye_power();
    if (stored != DEYE_MODE_NORMAL && stored < DEYE_MODE_COUNT) {
        s_undo_pending = true;
        s_undo_mode    = stored;
        s_mode         = (deye_mode_t)stored;   /* the inverter really is in it */
        if (stored_pw >= DEYE_POWER_MIN && stored_pw <= DEYE_POWER_MAX) {
            s_power_w = s_user_power_w = stored_pw;
        }
        ESP_LOGW(TAG, "stored mode '%s' (%u W) survived the restart in the inverter "
                      "-- undoing it shortly", deye_ctrl_mode_name((deye_mode_t)stored),
                 (unsigned)stored_pw);
    }
    s_mode_ms = now_ms();

    xTaskCreatePinnedToCore(deye_ctrl_task, "deye_ctrl", 4096, NULL, 4, &s_task, 0);
}

esp_err_t deye_ctrl_apply(deye_mode_t mode, int power_w)
{
    if (mode >= DEYE_MODE_COUNT) mode = DEYE_MODE_NORMAL;
    if (power_w < DEYE_POWER_MIN) power_w = DEYE_POWER_MIN;
    if (power_w > DEYE_POWER_MAX) power_w = DEYE_POWER_MAX;

    s_mode         = mode;
    s_power_w      = power_w;
    s_user_power_w = power_w;   /* user intent — never changed by the guard */
    s_mode_ms      = now_ms();

    /* Persist so a restart can UNDO a forced mode (deye_ctrl_start). NVS skips
     * writes of an unchanged value, so a repeated apply costs no flash. */
    nvs_store_set_deye_mode((uint8_t)mode);
    if (mode != DEYE_MODE_NORMAL) nvs_store_set_deye_power((uint16_t)power_w);

    ESP_LOGI(TAG, "apply mode=%s power=%d W (queued)", deye_ctrl_mode_name(mode), power_w);
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

void deye_ctrl_get_status(deye_ctrl_status_t *out)
{
    if (!out) return;
    uint32_t age = (uint32_t)(now_ms() - s_mode_ms) / 1000u;
    out->mode         = s_mode;
    out->power_w      = s_power_w;
    out->user_power_w = s_user_power_w;
    out->age_s        = age;
    out->left_s       = (s_mode == DEYE_MODE_NORMAL) ? 0
                        : (age >= DEYE_FORCE_MAX_S ? 0 : DEYE_FORCE_MAX_S - age);
    out->checked      = s_checked;
    out->failed       = s_failed;
}

esp_err_t deye_ctrl_set_throttled(int power_w)
{
    if (power_w < DEYE_POWER_MIN) power_w = DEYE_POWER_MIN;
    if (power_w > DEYE_POWER_MAX) power_w = DEYE_POWER_MAX;
    s_power_w = power_w;   /* s_user_power_w intentionally unchanged */
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

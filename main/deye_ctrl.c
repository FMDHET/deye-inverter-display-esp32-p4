#include "deye_ctrl.h"
#include "modbus_rtu.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "deye_ctrl";

/* Deye SG04LP3 control registers (write via FC16 over the RTU master bus). */
#define REG_WORK_MODE   142     /* 2 = Zero Export to CT, 3 = Selling First */
#define REG_SELL_POWER  143     /* Max Sell Power (W)                       */
#define REG_NORMAL_126  126     /* additional Normal-mode reset registers    */
#define REG_NORMAL_127  127
#define REG_NORMAL_128  128
#define DEYE_POWER_MIN  1000
#define DEYE_POWER_MAX  20000

/* Last requested mode + power magnitude (W).
 * s_user_power_w = what the user set (slider/MQTT); never changed by the guard.
 * s_power_w      = what is actually applied (may be throttled by SLS guard). */
static volatile deye_mode_t s_mode         = DEYE_MODE_NORMAL;
static volatile int         s_power_w      = 5000;
static volatile int         s_user_power_w = 5000;
static TaskHandle_t         s_task;

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
    switch (mode) {
    case DEYE_MODE_FORCE_CHARGE: {            /* charge: reg128 = charge current (A) @ ~50V */
        int rc3, rc4;
        int amps = power_w / 50;             /* WR ignores reg126(W); reacts to reg128 in A */
        rc1 = modbus_rtu_deye_write(REG_NORMAL_126, (uint16_t)power_w);
        rc3 = modbus_rtu_deye_write(REG_NORMAL_127, 99);
        rc4 = modbus_rtu_deye_write(REG_NORMAL_128, (uint16_t)amps);
        /* SOC set-points for the six time-of-use slots all to 99 % */
        for (int reg = 166; reg <= 171; reg++)
            modbus_rtu_deye_write(reg, 99);
        /* grid-charge enable flags for the six slots all ON */
        for (int reg = 172; reg <= 177; reg++)
            modbus_rtu_deye_write(reg, 1);
        ESP_LOGW(TAG, "Laden -> reg126=%d (rc=%d) reg127=99 (rc=%d) reg128=%dA (rc=%d) reg166-171=99 reg172-177=1",
                 power_w, rc1, rc3, amps, rc4);
        return;
    }
    case DEYE_MODE_FORCE_DISCHARGE:          /* discharge: Selling First @ power */
        rc1 = modbus_rtu_deye_write(REG_WORK_MODE,  3);
        rc2 = modbus_rtu_deye_write(REG_SELL_POWER, (uint16_t)power_w);
        break;
    case DEYE_MODE_NORMAL: {              /* back to Zero-Export-to-CT, full sell */
        int rc3, rc4, rc5;
        rc1 = modbus_rtu_deye_write(REG_WORK_MODE,  2);
        rc2 = modbus_rtu_deye_write(REG_SELL_POWER, 20000);
        rc3 = modbus_rtu_deye_write(REG_NORMAL_126, 5000);
        rc4 = modbus_rtu_deye_write(REG_NORMAL_127, 10);
        rc5 = modbus_rtu_deye_write(REG_NORMAL_128, 40);
        /* SOC set-points for the six time-of-use slots all back to 13 % */
        for (int reg = 166; reg <= 171; reg++)
            modbus_rtu_deye_write(reg, 13);
        /* grid-charge enable flags for the six slots all OFF */
        for (int reg = 172; reg <= 177; reg++)
            modbus_rtu_deye_write(reg, 0);
        ESP_LOGW(TAG, "Normal -> reg142=%d reg143=%d reg126=%d reg127=%d reg128=%d reg166-171=13 reg172-177=0",
                 rc1, rc2, rc3, rc4, rc5);
        return;
    }
    default:
        return;
    }
    ESP_LOGW(TAG, "%s -> reg142 rc=%d, reg143 rc=%d (power=%d W)",
             deye_ctrl_mode_name(mode), rc1, rc2, power_w);
}

static void deye_ctrl_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for an apply request */
        deye_ctrl_write_regs(s_mode, s_power_w);   /* latest request wins       */
    }
}

void deye_ctrl_start(void)
{
    /* core 0: keep the UI core (1) free; this task is mostly blocked on the RTU
     * bus anyway. Call AFTER modbus_rtu_start() (needs its request mutex). */
    if (!s_task)
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

    ESP_LOGI(TAG, "apply mode=%s power=%d W (queued)", deye_ctrl_mode_name(mode), power_w);
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t deye_ctrl_set_throttled(int power_w)
{
    if (power_w < DEYE_POWER_MIN) power_w = DEYE_POWER_MIN;
    if (power_w > DEYE_POWER_MAX) power_w = DEYE_POWER_MAX;
    s_power_w = power_w;   /* s_user_power_w intentionally unchanged */
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

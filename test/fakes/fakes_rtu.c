/* Fakes for the RS485 side that deye_ctrl.c talks to, plus the two NVS keys it
 * uses. Kept apart from fakes.c because that one is linked with the suite that
 * *includes* modbus_rtu.c -- the same symbols would collide.
 *
 * The register file is the point of this fake: the code under test writes a
 * register and reads it back to check the inverter really took it. So the fake
 * has to be able to say "written but not applied", which is precisely the
 * silent failure the read-back was added for.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "fakes_rtu.h"

/* The inverter's registers, as far as anyone here cares. */
static uint16_t s_regs[512];

fake_rtu_write_t fake_rtu_log[64];
int              fake_rtu_log_n;
int              fake_rtu_write_rc;      /* != 0 -> every write fails      */
int              fake_rtu_read_rc;       /* != 0 -> every read-back fails  */
bool             fake_rtu_deaf;          /* writes "succeed" but change nothing */

void fake_rtu_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    memset(fake_rtu_log, 0, sizeof(fake_rtu_log));
    fake_rtu_log_n    = 0;
    fake_rtu_write_rc = 0;
    fake_rtu_read_rc  = 0;
    fake_rtu_deaf     = false;
}

uint16_t fake_rtu_reg(uint16_t addr)
{
    return addr < 512 ? s_regs[addr] : 0;
}

void fake_rtu_set_reg(uint16_t addr, uint16_t val)
{
    if (addr < 512) s_regs[addr] = val;
}

int fake_rtu_wrote(uint16_t addr)
{
    int n = 0;
    for (int i = 0; i < fake_rtu_log_n; i++)
        if (fake_rtu_log[i].addr == addr) n++;
    return n;
}

int modbus_rtu_deye_write(uint16_t addr, uint16_t val)
{
    if (fake_rtu_log_n < (int)(sizeof(fake_rtu_log) / sizeof(fake_rtu_log[0]))) {
        fake_rtu_log[fake_rtu_log_n].addr = addr;
        fake_rtu_log[fake_rtu_log_n].val  = val;
        fake_rtu_log_n++;
    }
    if (fake_rtu_write_rc) return fake_rtu_write_rc;
    if (!fake_rtu_deaf && addr < 512) s_regs[addr] = val;
    return 0;
}

int modbus_rtu_deye_read(uint16_t addr, uint16_t count, uint16_t *out)
{
    if (fake_rtu_read_rc) return fake_rtu_read_rc;
    for (uint16_t i = 0; i < count; i++)
        out[i] = (addr + i) < 512 ? s_regs[addr + i] : 0;
    return 0;
}

/* --------------------------- NVS: forced mode -------------------------- */

extern bool fake_nvs_fail;               /* shared knob, defined in fakes.c */

static uint8_t  s_mode_stored;
static uint16_t s_power_stored;

void fake_deye_nvs_reset(void)
{
    s_mode_stored  = 0;
    s_power_stored = 0;
}

void fake_deye_nvs_set(uint8_t mode, uint16_t power)
{
    s_mode_stored  = mode;
    s_power_stored = power;
}

uint8_t   nvs_store_get_deye_mode(void)  { return s_mode_stored; }
uint16_t  nvs_store_get_deye_power(void) { return s_power_stored; }

esp_err_t nvs_store_set_deye_mode(uint8_t m)
{
    if (fake_nvs_fail) return ESP_FAIL;
    s_mode_stored = m;
    return ESP_OK;
}

esp_err_t nvs_store_set_deye_power(uint16_t w)
{
    if (fake_nvs_fail) return ESP_FAIL;
    s_power_stored = w;
    return ESP_OK;
}

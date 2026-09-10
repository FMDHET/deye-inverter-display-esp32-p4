#pragma once
/* Knobs for the RS485/inverter fake used by the deye_ctrl suite. */

#include <stdbool.h>
#include <stdint.h>

typedef struct { uint16_t addr, val; } fake_rtu_write_t;

extern fake_rtu_write_t fake_rtu_log[64];   /* every write, in order */
extern int              fake_rtu_log_n;
extern int              fake_rtu_write_rc;  /* != 0 -> every write fails       */
extern int              fake_rtu_read_rc;   /* != 0 -> every read-back fails   */
extern bool             fake_rtu_deaf;      /* writes "succeed", nothing changes */

void     fake_rtu_reset(void);
uint16_t fake_rtu_reg(uint16_t addr);
void     fake_rtu_set_reg(uint16_t addr, uint16_t val);
int      fake_rtu_wrote(uint16_t addr);     /* how often that register was written */

/* The stored forced mode, as if it had survived a restart. */
void fake_deye_nvs_reset(void);
void fake_deye_nvs_set(uint8_t mode, uint16_t power);

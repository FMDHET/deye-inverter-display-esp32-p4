#pragma once

/* The operator's discharge locks of the two batteries -- while the car
 * charges, say, so neither house battery ends up in the car. PV surplus still
 * charges them.
 *
 *  - Deye: through the meter emulation (modbus_rtu_set_lock), no register
 *    writes. Renewed every tick, expires BAT_LOCK_RENEW_S after the last one:
 *    a dead display leaves the battery free.
 *  - BYD: through the Fronius discharge block (fronius_storage_lock_discharge),
 *    next to and independent of the energy manager's block. This Fronius has
 *    no revert timer: the block holds until the ESP lifts it (also after a
 *    restart, see fronius_storage `owed`) -- or someone does in the Fronius.
 *
 * Each until lifted or for a set time, kept by this module (not by the
 * inverter) and in NVS, so a restart does not lift a lock. The energy
 * manager task calls bat_lock_tick() every ~10 s; set/get from any task. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BAT_DEYE = 0, BAT_BYD = 1, BAT_N } bat_t;

#define BAT_LOCK_RENEW_S   120          /* Deye hold lasts this long after a tick */
#define BAT_LOCK_MAX_MIN   (24 * 60)    /* longest timed lock                     */

typedef struct {
    bool    on;
    int64_t until;                      /* unix, 0 = until lifted                 */
} bat_lock_t;

/* Load the stored state. Call once before the first tick. */
void      bat_lock_init(void);

/* Lock (minutes 0 = until lifted, else 1..BAT_LOCK_MAX_MIN) or lift. A timed
 * lock needs the clock: ESP_ERR_INVALID_STATE before it is set. Takes effect
 * at once (Deye) or at the Fronius module's next tick (BYD). */
esp_err_t bat_lock_set(bat_t which, bool on, uint32_t minutes, int64_t now);

/* Renew the Deye hold, end timed locks that ran out, pass the BYD request
 * on. byd_allowed = the operator's "BYD control allowed": without it a BYD
 * lock is not passed on (and stays shown as asked for). */
void      bat_lock_tick(int64_t now, bool byd_allowed);

void      bat_lock_get(bat_t which, bat_lock_t *out);

#ifdef __cplusplus
}
#endif

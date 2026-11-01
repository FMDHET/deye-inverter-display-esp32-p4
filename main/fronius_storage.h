#pragma once

/* BYD battery behind a Fronius Symo Hybrid: read and (carefully) control it
 * over Modbus TCP, SunSpec model 124 "Storage".
 *
 * Why control at all: the BYD is the household's last-resort reserve. While
 * the energy manager charges the Deye battery from the grid, the Fronius would
 * otherwise cover that charging power from the BYD -- energy moved from one
 * battery into the other at a loss. A discharge block (OutWRte = 0 %) stops
 * that for the duration.
 *
 * Safety rules this module keeps:
 *  - nothing is ever written unless fronius_storage_block_discharge(),
 *    fronius_storage_force() or fronius_storage_set_reserve() asked for it;
 *  - every write is read back and compared -- an inverter that answers "ok"
 *    and keeps its old value is an error, not a success;
 *  - a block always carries the inverter's revert timer (InOutWRte_RvrtTms),
 *    refreshed while the block is wanted: if the display dies, the Fronius
 *    returns to its own limits by itself -- where it has one; the Symo Hybrid
 *    here does not (see no_revert), so the state is kept in NVS instead and
 *    put back after a restart;
 *  - a forced charge or discharge (the operator's, from the web page) always
 *    has an end kept by this module: a time and a SoC. It is never resumed
 *    after a restart of the display.
 *
 * Device: the first enabled Modbus-TCP device with manufacturer Fronius and
 * role inverter whose SunSpec map carries model 124. Each operation batch
 * borrows the connection the Modbus-TCP poll keeps to that address
 * (modbus_tcp_borrow) -- one client per datalogger, no second socket. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Revert time bounds for a discharge block (s). Below the minimum the 10-s
 * tick could not refresh in time; above the maximum a dead display would keep
 * the house battery locked for longer than anyone would accept. */
#define FRONIUS_STORAGE_RVRT_MIN_S   120
#define FRONIUS_STORAGE_RVRT_MAX_S   7200
#define FRONIUS_STORAGE_RVRT_DEF_S   1800

/* Forced operation of the BYD, set by the operator. */
typedef enum {
    FRONIUS_FORCE_OFF       = 0,
    FRONIUS_FORCE_CHARGE    = 1,   /* charge with exactly w, also from the grid */
    FRONIUS_FORCE_DISCHARGE = 2,   /* discharge with exactly w                  */
} fronius_force_t;

#define FRONIUS_FORCE_MIN_W      100
#define FRONIUS_FORCE_MAX_W      10000   /* clamped to WChaMax as well       */
#define FRONIUS_FORCE_DEF_MIN    60
#define FRONIUS_FORCE_MAX_MIN    720

typedef struct {
    bool     ok;                 /* last read succeeded                     */
    char     ip[32];
    uint16_t addr;               /* model 124 start, 0 = not found          */
    float    soc_pct, reserve_pct, wchamax_w;
    float    out_rte_pct, in_rte_pct;   /* discharge / charge limits, %     */
    uint16_t ctl_mod, rvrt_s, cha_st, grid_charge;
    bool     blocked;            /* our discharge block is in force         */
    int64_t  read_at;            /* unix */
    char     err[80];            /* German, "" = none                       */
    /* The inverter ignores InOutWRte_RvrtTms (reads 0xFFFF = "not
     * implemented"): a block does NOT lift itself if this display dies --
     * only our own release (also after a restart, see `owed`) does. */
    bool     no_revert;
    bool     owed;               /* a setting of ours still has to be undone */
    /* forced operation */
    bool     forcing;            /* a forced mode is in force (verified)    */
    uint8_t  force;              /* requested: fronius_force_t              */
    uint16_t force_w;
    int64_t  force_until;        /* unix, 0 = none                          */
    float    force_soc;          /* ends at this SoC (charge >=, discharge <=) */
    char     force_end[48];      /* why the last one ended, German          */
    /* While a setting of ours is in force: the operator's own values that
     * come back when it ends (raw registers, rates in % x 100 at the
     * device's scale -2; base_in 0xFFFF = not known). */
    uint8_t  cap_pct;            /* upper charge limit asked for, 0 = off   */
    bool     lock_req;           /* the operator's discharge lock is asked for */
    bool     cap_on;             /* charging held at the cap now (verified) */
    bool     base_known;
    uint16_t base_ctl, base_out, base_in;
} fronius_storage_status_t;

/* The operator's settings the Fronius card can change by hand. */
typedef enum {
    FRONIUS_REG_RESERVE = 0,     /* MinRsvPct, 0..100 %                     */
    FRONIUS_REG_OUT,             /* OutWRte, discharge limit 0..100 %       */
    FRONIUS_REG_IN,              /* InWRte, charge limit 0..100 %           */
    FRONIUS_REG_CTL,             /* StorCtl_Mod, 0..3 (bit0 charge, bit1 discharge limit on) */
    FRONIUS_REG_GRID,            /* ChaGriSet, 0 = PV only, 1 = grid too    */
} fronius_reg_t;

/* Call every ~10 s from the energy manager task (`now` = unix seconds). Reads
 * the status every 30 s, applies a changed block request at once and refreshes
 * an active block's revert timer. Blocks for up to a few seconds (network). */
void      fronius_storage_tick(int64_t now);

/* Copy of the last status (spinlock, never blocks). */
void      fronius_storage_get(fronius_storage_status_t *out);

/* Request a discharge block (on) or its release (off). Applied and verified in
 * the next tick; the outcome shows in status.blocked / status.err. revert_s is
 * clamped to [FRONIUS_STORAGE_RVRT_MIN_S, FRONIUS_STORAGE_RVRT_MAX_S]
 * (0 = default); ignored for off. */
esp_err_t fronius_storage_block_discharge(bool on, uint16_t revert_s);

/* Start a forced charge/discharge with `w` watts for `minutes` (0 = default)
 * or until the BYD reaches `soc_pct`, whichever comes first; OFF ends one.
 * Wins over the discharge block while it runs; afterwards the block (if the
 * energy manager still wants it) or the originals come back. Applied and
 * verified in the next tick. ESP_ERR_INVALID_ARG for values out of range. */
esp_err_t fronius_storage_force(fronius_force_t mode, uint16_t w, uint16_t minutes,
                                float soc_pct, int64_t now);

/* The operator's discharge lock (bat_lock.c keeps its duration): a second
 * request source next to the energy manager's block -- the BYD is blocked
 * while either asks, and neither lifts the other's. */
void      fronius_storage_lock_discharge(bool on);

/* Upper charge limit of the BYD, enforced by this module: at max_soc_pct
 * charging is held (InWRte 0 %), 2 points below it released (0 or >= 100 =
 * off). SunSpec model 124 has no maximum SoC of its own. A forced mode is
 * not capped -- it has its own SoC target. */
void      fronius_storage_set_charge_cap(uint8_t max_soc_pct);

/* End a forced mode, with the reason shown as force_end (German). No-op when
 * none runs. */
void      fronius_storage_force_stop(const char *why);

/* Set one of the operator's settings and verify it by read-back
 * (synchronous, a few seconds). While a block or forced mode of ours drives
 * that register, the value is stored as the base value restored when ours
 * ends instead (*as_base = true). Rates are 0..100 % here: negative ones are
 * forced operation, see fronius_storage_force(). */
esp_err_t fronius_storage_set(fronius_reg_t reg, float val, bool *as_base);

/* Write the minimum reserve (MinRsvPct, 0..100 %) and verify it by read-back.
 * Synchronous (network, up to a few seconds). ESP_ERR_INVALID_STATE before the
 * first tick. */
esp_err_t fronius_storage_set_reserve(float pct);

/* A setting of ours is still in the inverter, or a forced mode or the
 * charge cap is requested (also one that survived a restart
 * of the display: the state lives in NVS). The energy manager keeps ticking
 * the module while this is true, so the release goes out without anyone
 * looking at the page. */
bool      fronius_storage_owed(void);

/* False while InWRte is ours (a forced mode ran and is not put back yet):
 * firmware from before forced modes would release the block bits but leave
 * InWRte at the forced value. The OTA rollback refuses until then. */
bool      fronius_storage_rollback_safe(void);

#ifdef __cplusplus
}
#endif

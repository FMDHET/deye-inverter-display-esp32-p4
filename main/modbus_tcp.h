#pragma once

/* Multi-device Modbus-TCP manager.
 *
 * Each device is configured by MANUFACTURER (protocol/register profile) + ROLE
 * (which UI value it feeds) + IP/port/slave-id. The values are aggregated into
 * the energy-flow UI. A Fronius hybrid INVERTER additionally reads its battery
 * (SunSpec storage model 124 = SoC, MPPT model 160 = power) -> BYD node.
 *
 * The device list is edited in the "Mod TCP" settings tab and stored in NVS.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_MAX_DEVICES 8

/* Manufacturer -> protocol / register map. */
typedef enum {
    MB_MFR_FRONIUS = 0,   /* SunSpec (FC03)            */
    MB_MFR_DEYE,          /* Deye SG04LP3 native (FC03) */
    MB_MFR_ELTAKO,        /* Eltako DSZ15/DSZ16 (FC04 int32 watts) */
    MB_MFR_COUNT
} mb_mfr_t;

/* Device role -> which energy-flow value it provides. */
typedef enum {
    MB_ROLE_GRID = 0,     /* Zähler am Netzübergabepunkt -> Netz        */
    MB_ROLE_PRODMETER,    /* Zähler vor dem Wechselrichter -> PV         */
    MB_ROLE_INVERTER,     /* Wechselrichter (Fronius:+BYD, Deye:+Akku)   */
    MB_ROLE_BATTERY,      /* dedizierte Batterie -> Akku                 */
    MB_ROLE_DEYE_METER,   /* Zähler vor dem Deye-WR -> Deye-AC-Leistung  */
    MB_ROLE_COUNT
} mb_role_t;

/* Plain-old-data, persisted to NVS as a blob.
 * NOTE: fields are APPENDED so each new field migrates in-place from the
 * previous NVS layout -- see load_cfg() in modbus_tcp.c. */
typedef struct {
    char     ip[32];
    uint16_t port;        /* default 502         */
    uint8_t  slave;       /* Modbus slave/unit id */
    uint8_t  mfr;         /* mb_mfr_t            */
    uint8_t  role;        /* mb_role_t           */
    uint8_t  enabled;     /* 0/1                 */
    uint16_t poll_ms;     /* per-device poll interval (0 -> default 2000) */
    uint16_t timeout_ms;  /* connect/liveness timeout (0 -> default 500); reads
                             get a >=2s floor for slow dataloggers */
    char     name[24];    /* user label shown in the UI (APPENDED -> devs5) */
} mb_dev_cfg_t;

#define MB_DEFAULT_POLL_MS    2000
#define MB_MIN_POLL_MS        200
#define MB_MAX_POLL_MS        60000
#define MB_DEFAULT_TIMEOUT_MS 500
#define MB_MIN_TIMEOUT_MS     100
#define MB_MAX_TIMEOUT_MS     10000

typedef struct {
    uint8_t  dev_count;
    uint8_t  connected;
    uint32_t poll_count;
    uint32_t err_count;
    float    pv_w, house_w, grid_w;
    float    deye_w, deye_soc;
    float    byd_w,  byd_soc;
} modbus_tcp_status_t;

esp_err_t modbus_tcp_start(void);
void      modbus_tcp_reconfigure(void);
void      modbus_tcp_get_status(modbus_tcp_status_t *out);

/* Latest grid power (W, +import / -export), but ONLY if it was refreshed from a
 * real meter read within max_age_ms. Returns false when no grid source exists,
 * the value is stale (poll task slow/stalled), or nothing has been read since
 * (re)config. Anything that STEERS the inverter (the RTU Eastron emulation)
 * MUST gate on this and stay passive on false -- never regulate against a
 * frozen value (that caused a 15 kW export runaway). */
bool      modbus_tcp_grid_w_fresh(float *out_w, uint32_t max_age_ms);

/* Deye values supplied by the RTU master (modbus_rtu.c) instead of TCP. They
 * feed the Deye node + house balance; SoC also comes from here. */
void      modbus_tcp_set_rtu_deye(float w, float soc, bool valid);

/* Per-device live values (for drill-down popups, e.g. individual inverters). */
typedef struct {
    char    ip[32];
    char    name[24];    /* user label (empty -> fall back to mfr/ip) */
    uint8_t slave;       /* Modbus slave/unit id */
    uint8_t mfr;         /* mb_mfr_t  */
    uint8_t role;        /* mb_role_t */
    bool    connected;
    float   pv_w;        /* PV contribution                 */
    float   w;           /* battery or grid power           */
    float   soc;         /* battery SoC %                   */
} mb_dev_live_t;

/* Device-list access for the settings UI. */
int         modbus_tcp_get_devices(mb_dev_cfg_t *out, int max);   /* -> count */
esp_err_t   modbus_tcp_set_devices(const mb_dev_cfg_t *list, int count);
const char *modbus_tcp_mfr_name(uint8_t mfr);
const char *modbus_tcp_role_name(uint8_t role);

/* Snapshot of live per-device values. Returns the count copied. */
int         modbus_tcp_get_device_live(mb_dev_live_t *out, int max);

#ifdef __cplusplus
}
#endif

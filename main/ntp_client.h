#pragma once

/* SNTP time client. Keeps the system clock in sync from a configurable NTP
 * server and applies a selectable POSIX timezone so localtime() reflects the
 * wall-clock time shown on the main screen. Config persists in NVS. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persisted clock configuration (stored as an opaque NVS blob). */
typedef struct {
    uint8_t enabled;       /* SNTP on/off                     */
    uint8_t tz_idx;        /* index into the timezone table   */
    char    server[64];    /* NTP server hostname             */
} ntp_cfg_t;

/* Selectable timezones (offset+location name + POSIX TZ string). */
#define NTP_TZ_COUNT   13
#define NTP_TZ_DEFAULT 5      /* UTC+1 Berlin */
const char *ntp_tz_name(int idx);     /* e.g. "UTC+1  Berlin"              */
const char *ntp_tz_posix(int idx);    /* e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */

/* Start SNTP (call once after WiFi is up). Reads config from NVS. */
esp_err_t ntp_start(void);

/* Settings-UI accessors (lazy-load from NVS on first use, like mqtt_fwd). */
void      ntp_get_cfg(ntp_cfg_t *out);
esp_err_t ntp_set_cfg(const ntp_cfg_t *cfg);

/* True once the clock has been set from the network at least once. */
bool      ntp_is_synced(void);

#ifdef __cplusplus
}
#endif

#pragma once

/* Forward the live energy values to an MQTT broker, with optional Home
 * Assistant MQTT-discovery, retain flag and Last-Will. Config is stored in NVS
 * and edited in the "MQTT" settings tab. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  enabled;
    char     host[64];     /* broker host / IP            */
    uint16_t port;         /* default 1883                */
    char     user[40];
    char     pass[40];
    char     base[32];     /* base topic, default "deye-display" */
    uint8_t  retain;       /* retain state publishes      */
    uint8_t  discovery;    /* publish HA MQTT-discovery    */
    uint8_t  lastwill;     /* register a Last-Will         */
} mqtt_cfg_t;

typedef struct {
    bool     enabled;
    bool     connected;
    uint32_t published;
} mqtt_fwd_status_t;

esp_err_t mqtt_fwd_start(void);
void      mqtt_fwd_get_cfg(mqtt_cfg_t *out);
esp_err_t mqtt_fwd_set_cfg(const mqtt_cfg_t *cfg);
void      mqtt_fwd_get_status(mqtt_fwd_status_t *out);

#ifdef __cplusplus
}
#endif

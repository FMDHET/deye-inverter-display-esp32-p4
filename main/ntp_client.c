#include "ntp_client.h"
#include "nvs_store.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ntp";

/* Offset+location label + POSIX TZ string for each selectable zone, ordered
 * west -> east. The offset shown is standard time; the POSIX strings encode the
 * DST rules so localtime() switches automatically where applicable. Keep
 * NTP_TZ_DEFAULT (header) pointing at the Berlin row. */
static const struct { const char *name, *posix; } TZ[NTP_TZ_COUNT] = {
    { "UTC-8  Los Angeles", "PST8PDT,M3.2.0,M11.1.0"        },  /* 0 */
    { "UTC-5  New York",    "EST5EDT,M3.2.0,M11.1.0"        },  /* 1 */
    { "UTC-3  Sao Paulo",   "<-03>3"                        },  /* 2 */
    { "UTC+0  London",      "GMT0BST,M3.5.0/1,M10.5.0"      },  /* 3 */
    { "UTC+0  UTC",         "UTC0"                          },  /* 4 */
    { "UTC+1  Berlin",      "CET-1CEST,M3.5.0,M10.5.0/3"    },  /* 5 (default) */
    { "UTC+2  Athen",       "EET-2EEST,M3.5.0/3,M10.5.0/4"  },  /* 6 */
    { "UTC+3  Moskau",      "MSK-3"                         },  /* 7 */
    { "UTC+4  Dubai",       "<+04>-4"                       },  /* 8 */
    { "UTC+5:30  Neu-Delhi","IST-5:30"                      },  /* 9 */
    { "UTC+8  Singapur",    "<+08>-8"                       },  /* 10 */
    { "UTC+9  Tokio",       "JST-9"                         },  /* 11 */
    { "UTC+10  Sydney",     "AEST-10AEDT,M10.1.0,M4.1.0/3"  },  /* 12 */
};

const char *ntp_tz_name(int idx)
{
    return (idx >= 0 && idx < NTP_TZ_COUNT) ? TZ[idx].name : TZ[0].name;
}
const char *ntp_tz_posix(int idx)
{
    return (idx >= 0 && idx < NTP_TZ_COUNT) ? TZ[idx].posix : TZ[0].posix;
}

#define DEF_SERVER "pool.ntp.org"

static ntp_cfg_t      s_cfg;
static bool           s_loaded;
static volatile bool  s_synced;
static bool           s_running;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;

static void load_cfg(void)
{
    ntp_cfg_t c;
    memset(&c, 0, sizeof(c));
    if (nvs_store_get_ntp(&c, sizeof(c)) != ESP_OK) {
        c.enabled = 1;
        c.tz_idx  = NTP_TZ_DEFAULT;
        strncpy(c.server, DEF_SERVER, sizeof(c.server) - 1);
    }
    if (c.tz_idx >= NTP_TZ_COUNT)  c.tz_idx = NTP_TZ_DEFAULT;
    if (c.server[0] == '\0')       strncpy(c.server, DEF_SERVER, sizeof(c.server) - 1);
    portENTER_CRITICAL(&s_mux);
    s_cfg = c;
    s_loaded = true;
    portEXIT_CRITICAL(&s_mux);
}

static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "clock synchronised");
}

/* (Re)start the SNTP service from the current config. Always (re)applies the
 * timezone so the displayed local time is correct once a sync arrives. */
static void apply(void)
{
    if (s_running) {
        esp_netif_sntp_deinit();
        s_running = false;
    }
    s_synced = false;

    setenv("TZ", ntp_tz_posix(s_cfg.tz_idx), 1);
    tzset();

    if (!s_cfg.enabled || s_cfg.server[0] == '\0') {
        ESP_LOGI(TAG, "disabled");
        return;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_cfg.server);
    cfg.sync_cb = on_sync;
    esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "sntp init: %s", esp_err_to_name(e));
        return;
    }
    s_running = true;
    ESP_LOGI(TAG, "sntp -> %s  (tz %s)", s_cfg.server, ntp_tz_posix(s_cfg.tz_idx));
}

esp_err_t ntp_start(void)
{
    if (!s_loaded) load_cfg();
    apply();
    return ESP_OK;
}

void ntp_get_cfg(ntp_cfg_t *out)
{
    if (!out) return;
    if (!s_loaded) load_cfg();
    portENTER_CRITICAL(&s_mux); *out = s_cfg; portEXIT_CRITICAL(&s_mux);
}

esp_err_t ntp_set_cfg(const ntp_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    esp_err_t e = nvs_store_set_ntp(cfg, sizeof(*cfg));
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux); s_cfg = *cfg; s_loaded = true; portEXIT_CRITICAL(&s_mux);
        apply();
    }
    return e;
}

bool ntp_is_synced(void)
{
    return s_synced;
}

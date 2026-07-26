#include "wg_client.h"
#include "nvs_store.h"
#include "ntp_client.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wg";

#define DEF_PORT       51820
#define DEF_KEEPALIVE  25

static wg_cfg_t            s_cfg;
static bool               s_loaded;
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;

static wireguard_config_t s_wgcfg;     /* must outlive the connection */
static wireguard_ctx_t    s_ctx;
static bool               s_inited;    /* esp_wireguard_init done      */
static volatile bool      s_restart;   /* set by wg_set_cfg            */
static volatile bool      s_up;        /* peer handshake established    */

static void load_cfg(void)
{
    wg_cfg_t c;
    memset(&c, 0, sizeof(c));
    if (nvs_store_get_wg(&c, sizeof(c)) != ESP_OK) {
        c.enabled   = 0;
        c.port      = DEF_PORT;
        c.keepalive = DEF_KEEPALIVE;
        strncpy(c.netmask, "255.255.255.0", sizeof(c.netmask) - 1);
    }
    if (c.port == 0)        c.port = DEF_PORT;
    if (c.netmask[0] == '\0') strncpy(c.netmask, "255.255.255.0", sizeof(c.netmask) - 1);
    portENTER_CRITICAL(&s_mux);
    s_cfg = c;
    s_loaded = true;
    portEXIT_CRITICAL(&s_mux);
}

/* (Re)bring up the tunnel from the current config. Runs only on the wg task. */
static void wg_apply(void)
{
    if (s_inited) {
        esp_wireguard_disconnect(&s_ctx);
        s_inited = false;
        s_up = false;
    }

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "disabled");
        return;
    }
    if (!s_cfg.private_key[0] || !s_cfg.public_key[0] ||
        !s_cfg.endpoint[0]    || !s_cfg.address[0]) {
        ESP_LOGW(TAG, "incomplete config (need private/public key, endpoint, tunnel IP)");
        return;
    }
    /* A WireGuard base64 key is exactly 44 chars (32 bytes). Reject anything
     * else BEFORE esp_wireguard decodes it into a fixed 32-byte buffer -- a too
     * long key (e.g. an accidental paste) would otherwise overflow it. */
    if (strlen(s_cfg.private_key) != 44 || strlen(s_cfg.public_key) != 44 ||
        (s_cfg.preshared_key[0] && strlen(s_cfg.preshared_key) != 44)) {
        ESP_LOGE(TAG, "invalid key length (keys must be 44-char base64) -- not connecting");
        return;
    }

    memset(&s_wgcfg, 0, sizeof(s_wgcfg));
    s_wgcfg.private_key          = s_cfg.private_key;
    s_wgcfg.public_key           = s_cfg.public_key;
    s_wgcfg.preshared_key        = s_cfg.preshared_key[0] ? s_cfg.preshared_key : NULL;
    s_wgcfg.allowed_ip           = s_cfg.address;       /* this device's tunnel IP */
    s_wgcfg.allowed_ip_mask      = s_cfg.netmask;
    s_wgcfg.endpoint             = s_cfg.endpoint;
    s_wgcfg.port                 = s_cfg.port ? s_cfg.port : DEF_PORT;
    s_wgcfg.persistent_keepalive = s_cfg.keepalive;
    s_wgcfg.listen_port          = 0;

    esp_err_t e = esp_wireguard_init(&s_wgcfg, &s_ctx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "init: %s", esp_err_to_name(e)); return; }
    e = esp_wireguard_connect(&s_ctx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "connect: %s", esp_err_to_name(e)); return; }
    s_inited = true;
    ESP_LOGI(TAG, "tunnel up: %s/%s -> %s:%u",
             s_cfg.address, s_cfg.netmask, s_cfg.endpoint, s_wgcfg.port);
}

static void wg_task(void *arg)
{
    (void)arg;
    load_cfg();

    /* The handshake embeds a timestamp; wait (bounded) for NTP before the
     * first attempt so the peer doesn't reject us. */
    for (int i = 0; i < 60 && s_cfg.enabled && !ntp_is_synced(); i++)
        vTaskDelay(pdMS_TO_TICKS(500));

    wg_apply();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_restart) {
            s_restart = false;
            wg_apply();
        }
        if (s_inited) {
            s_up = (esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK);
        }
    }
}

/* ------------------------------ public -------------------------------- */

esp_err_t wg_start(void)
{
    if (xTaskCreate(wg_task, "wg", 8192, NULL, 4, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}

void wg_get_cfg(wg_cfg_t *out)
{
    if (!out) return;
    if (!s_loaded) load_cfg();
    portENTER_CRITICAL(&s_mux); *out = s_cfg; portEXIT_CRITICAL(&s_mux);
}

esp_err_t wg_set_cfg(const wg_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    esp_err_t e = nvs_store_set_wg(cfg, sizeof(*cfg));
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux); s_cfg = *cfg; s_loaded = true; portEXIT_CRITICAL(&s_mux);
        s_restart = true;     /* applied by the wg task */
    }
    return e;
}

void wg_get_status(wg_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    out->enabled = s_cfg.enabled;
    strncpy(out->address, s_cfg.address, sizeof(out->address) - 1);
    out->address[sizeof(out->address) - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);
    out->up = s_up;
}

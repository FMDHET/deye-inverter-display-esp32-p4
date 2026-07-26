#include "wifi_mgr.h"
#include "nvs_store.h"

#include <string.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wifi_mgr";

#define STA_CONNECT_TIMEOUT_MS  20000   /* fall back to AP after this */
#define STA_RETRY_PERIOD_MS     15000   /* keep retrying in background */
#define AP_DEFAULT_CHANNEL      6
#define SCAN_MAX_APS            32

static wifi_mgr_state_t s_state;
static esp_netif_t     *s_sta_if;
static esp_netif_t     *s_ap_if;
static char             s_sta_ssid[33];
static wifi_cred_t      s_creds[WIFI_MGR_MAX_CREDS];
static int              s_cred_count;
static int              s_try_idx;      /* saved-cred index being attempted */
static bool             s_fast;         /* true = fast boot burst, false = slow retry */
static char             s_ap_ssid[33];
static char             s_sta_ip[16];
static char             s_ap_ip[16];
static int8_t           s_rssi;
static bool             s_ap_active;
static bool             s_scan_busy;
static wifi_mgr_ap_t    s_scan_results[SCAN_MAX_APS];
static size_t           s_scan_count;
static TimerHandle_t    s_fallback_timer;
static TimerHandle_t    s_reconnect_timer;   /* throttled STA retry in AP fallback */

/* ----------------------------- helpers --------------------------------- */

static void build_ap_ssid(char *out, size_t outsz)
{
    uint8_t mac[6] = {0};
    /* On ESP-Hosted the WiFi MAC lives on the C6. ESP_MAC_WIFI_SOFTAP
     * can't be derived on the P4 (logs "mac type is incorrect"); the
     * base/STA MAC is forwarded from the slave and works for naming. */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(out, outsz, "DeyeDisplay-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void fmt_ip(esp_netif_ip_info_t *info, char *out, size_t outsz)
{
    snprintf(out, outsz, IPSTR, IP2STR(&info->ip));
}

/* ----------------------------- AP start -------------------------------- */

static esp_err_t start_ap(void)
{
    char psk[64];
    nvs_store_get_ap_psk(psk, sizeof(psk));
    bool open = (strlen(psk) < 8);

    build_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, s_ap_ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len      = strlen(s_ap_ssid);
    wc.ap.channel       = AP_DEFAULT_CHANNEL;
    wc.ap.max_connection = 4;
    wc.ap.authmode      = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if (!open) {
        strncpy((char *)wc.ap.password, psk, sizeof(wc.ap.password) - 1);
    }

    /* The AP interface can only be configured once it is enabled, so switch
     * the mode FIRST (STA -> APSTA on fallback, NULL -> AP on cold start),
     * then apply the AP config. */
    wifi_mode_t cur = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur);
    if (cur == WIFI_MODE_STA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else if (cur == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "ap cfg");

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_if, &ip) == ESP_OK) {
        fmt_ip(&ip, s_ap_ip, sizeof(s_ap_ip));
    }
    s_ap_active = true;
    ESP_LOGI(TAG, "AP up: SSID=%s, IP=%s, %s", s_ap_ssid, s_ap_ip,
             open ? "OPEN" : "WPA2");
    return ESP_OK;
}

/* ----------------------------- STA start ------------------------------- */

static esp_err_t apply_sta_cfg(const char *ssid, const char *psk)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (psk && psk[0] != '\0') {
        strncpy((char *)wc.sta.password, psk, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    /* Scan all channels and pick the AP with the strongest signal -- relevant
     * when the same SSID is served by multiple access points. Default
     * (WIFI_FAST_SCAN) would just take the first match found. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    return esp_wifi_set_config(WIFI_IF_STA, &wc);
}

/* ----------------------------- timer cb -------------------------------- */

/* esp_wifi / esp_hosted RPC calls are far too stack-hungry to run in the
 * FreeRTOS timer-service task (~2 KB stack) -- doing so overflows it and
 * panics (stack protection fault). The timer callbacks therefore only poke a
 * dedicated worker task with a generous stack, which does the real work. */
#define WMGR_NOTIFY_FALLBACK_AP  (1u << 0)
#define WMGR_NOTIFY_RECONNECT    (1u << 1)
#define WMGR_NOTIFY_CONNECT      (1u << 2)

static TaskHandle_t s_worker;

/* Heavy esp_wifi calls (set_config/connect) -- run only from a task with a big
 * stack (worker / app_main / httpd / lvgl), never the event-loop/timer task. */
static void do_connect(int idx);

static void wifi_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, portMAX_DELAY);

        if (bits & WMGR_NOTIFY_FALLBACK_AP) {
            if (s_state == WIFI_MGR_STA_CONNECTING) {
                ESP_LOGW(TAG, "STA connect timeout - falling back to AP");
                s_state = WIFI_MGR_AP_FALLBACK;
                s_fast  = false;
                start_ap();
                if (s_reconnect_timer) xTimerStart(s_reconnect_timer, 0);
            }
        }
        if (bits & WMGR_NOTIFY_CONNECT) {
            do_connect(s_try_idx);          /* index chosen by the event handler */
        }
        if (bits & WMGR_NOTIFY_RECONNECT) {
            /* Slow single retry: step to the next saved network and try one. */
            if (s_cred_count > 0) {
                int idx = s_try_idx + 1;
                if (idx >= s_cred_count) idx = 0;
                ESP_LOGI(TAG, "throttled STA retry");
                do_connect(idx);
            }
        }
    }
}

/* ---- saved-credential helpers (defined here: need s_worker + notify bits) -- */
static int find_cred(const char *ssid)
{
    for (int i = 0; i < s_cred_count; i++)
        if (strncmp(s_creds[i].ssid, ssid, sizeof(s_creds[i].ssid)) == 0) return i;
    return -1;
}

static void save_creds(void)
{
    if (s_cred_count > 0)
        nvs_store_set_wifi_list(s_creds, (size_t)s_cred_count * sizeof(wifi_cred_t));
    else
        nvs_store_clear_wifi_list();
}

static void load_creds(void)
{
    size_t len = sizeof(s_creds);
    memset(s_creds, 0, sizeof(s_creds));
    if (nvs_store_get_wifi_list(s_creds, &len) == ESP_OK && len >= sizeof(wifi_cred_t)) {
        s_cred_count = (int)(len / sizeof(wifi_cred_t));
        if (s_cred_count > WIFI_MGR_MAX_CREDS) s_cred_count = WIFI_MGR_MAX_CREDS;
    } else {
        s_cred_count = 0;
    }
    /* One-time migration from the legacy single-credential keys. */
    if (s_cred_count == 0) {
        char ssid[33] = {0}, psk[65] = {0};
        if (nvs_store_get_sta(ssid, sizeof(ssid), psk, sizeof(psk)) == ESP_OK && ssid[0]) {
            strncpy(s_creds[0].ssid, ssid, sizeof(s_creds[0].ssid) - 1);
            strncpy(s_creds[0].psk,  psk,  sizeof(s_creds[0].psk)  - 1);
            s_cred_count = 1;
            save_creds();
        }
    }
    ESP_LOGI(TAG, "%d saved WiFi network(s)", s_cred_count);
}

static void do_connect(int idx)
{
    if (idx < 0 || idx >= s_cred_count) return;
    s_try_idx = idx;
    strncpy(s_sta_ssid, s_creds[idx].ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    ESP_LOGI(TAG, "STA try [%d/%d] %s", idx + 1, s_cred_count, s_creds[idx].ssid);
    apply_sta_cfg(s_creds[idx].ssid, s_creds[idx].psk);
    esp_wifi_connect();
}

/* Ask the worker to connect to saved index `idx` (safe from the event loop). */
static void request_connect(int idx)
{
    s_try_idx = idx;
    if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_CONNECT, eSetBits);
}

static void fallback_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_FALLBACK_AP, eSetBits);
}

/* One-shot: a single slow STA reconnect attempt. Used while the SoftAP is up
 * so we don't hammer the shared 2.4 GHz radio and starve AP clients. */
static void reconnect_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_RECONNECT, eSetBits);
}

/* ----------------------------- event handler --------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* Connects are driven explicitly via do_connect()/request_connect()
             * so we pick the right saved network -- don't auto-connect here
             * (would fire with stale/empty cfg, e.g. when scanning in AP mode). */
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_sta_ip[0] = '\0';
            s_rssi = 0;
            if (s_state == WIFI_MGR_STA_CONNECTED) {
                /* Lost an established link -> re-select from the top of the list. */
                ESP_LOGW(TAG, "STA disconnected, re-selecting");
                s_state = WIFI_MGR_STA_CONNECTING;
                s_fast  = true;
                if (s_fallback_timer) {
                    xTimerReset(s_fallback_timer, 0);
                    xTimerStart(s_fallback_timer, 0);
                }
                request_connect(0);
            } else if (s_state == WIFI_MGR_STA_CONNECTING && s_fast) {
                /* Boot burst: this candidate failed, try the next saved one. */
                int next = s_try_idx + 1;
                if (next < s_cred_count) {
                    request_connect(next);
                } else {
                    /* Whole list tried once without success -> retry slowly. */
                    s_fast = false;
                    if (s_reconnect_timer) xTimerStart(s_reconnect_timer, 0);
                }
            } else {
                /* Slow phase (AP fallback up, or post-burst): one retry per tick
                 * so we don't keep knocking the SoftAP off the shared radio. */
                if (s_reconnect_timer) xTimerStart(s_reconnect_timer, 0);
            }
            break;
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = SCAN_MAX_APS;
            /* static, NOT on the stack: this handler runs on the small
             * sys_evt task stack (~2.3 KB) and wifi_ap_record_t[32] is
             * ~3 KB -- a stack array here overflows it and panics. */
            static wifi_ap_record_t recs[SCAN_MAX_APS];
            esp_wifi_scan_get_ap_records(&n, recs);
            s_scan_count = 0;
            for (uint16_t i = 0; i < n && i < SCAN_MAX_APS; i++) {
                strncpy(s_scan_results[i].ssid, (char *)recs[i].ssid,
                        sizeof(s_scan_results[i].ssid) - 1);
                memcpy(s_scan_results[i].bssid, recs[i].bssid, 6);
                s_scan_results[i].rssi     = recs[i].rssi;
                s_scan_results[i].channel  = recs[i].primary;
                s_scan_results[i].authmode = recs[i].authmode;
                s_scan_count++;
            }
            s_scan_busy = false;
            ESP_LOGI(TAG, "scan done, %u APs", (unsigned)s_scan_count);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "AP client " MACSTR " joined", MAC2STR(e->mac));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *evt = data;
            fmt_ip(&evt->ip_info, s_sta_ip, sizeof(s_sta_ip));
            wifi_ap_record_t info;
            if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) s_rssi = info.rssi;
            s_state = WIFI_MGR_STA_CONNECTED;
            s_fast  = false;
            if (s_fallback_timer) xTimerStop(s_fallback_timer, 0);
            if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
            ESP_LOGI(TAG, "STA got IP %s on %s, RSSI %d", s_sta_ip, s_sta_ssid, s_rssi);
        }
    }
}

/* ----------------------------- public API ------------------------------ */

esp_err_t wifi_mgr_init(void)
{
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "hosted");
    ESP_RETURN_ON_ERROR(esp_netif_init(),  TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    s_sta_if = esp_netif_create_default_wifi_sta();
    s_ap_if  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wc), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
        TAG, "ev wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL),
        TAG, "ev ip");

    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    s_fallback_timer = xTimerCreate("wifi_fb",
                                    pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS),
                                    pdFALSE, NULL, fallback_timer_cb);
    s_reconnect_timer = xTimerCreate("wifi_rc",
                                    pdMS_TO_TICKS(STA_RETRY_PERIOD_MS),
                                    pdFALSE, NULL, reconnect_timer_cb);

    /* Worker that runs the stack-heavy WiFi actions off the timer task. */
    xTaskCreate(wifi_worker_task, "wifi_mgr_wk", 5120, NULL, 5, &s_worker);

    load_creds();
    if (s_cred_count > 0) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_state = WIFI_MGR_STA_CONNECTING;
        s_fast  = true;
        do_connect(0);                 /* runs on app_main task (big stack) */
        if (s_fallback_timer) xTimerStart(s_fallback_timer, 0);
    } else {
        esp_wifi_set_mode(WIFI_MODE_AP);
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        start_ap();
        s_state = WIFI_MGR_AP_ONLY;
    }
    return ESP_OK;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state     = s_state;
    out->rssi      = s_rssi;
    out->ap_active = s_ap_active;
    strncpy(out->sta_ssid, s_sta_ssid, sizeof(out->sta_ssid) - 1);
    strncpy(out->sta_ip,   s_sta_ip,   sizeof(out->sta_ip)   - 1);
    strncpy(out->ap_ssid,  s_ap_ssid,  sizeof(out->ap_ssid)  - 1);
    strncpy(out->ap_ip,    s_ap_ip,    sizeof(out->ap_ip)    - 1);

    /* On ESP-Hosted the WiFi MAC lives on the C6, so esp_read_mac() on the P4
     * returns the wrong/no MAC. Query the actual STA interface MAC over the
     * RPC and cache it (it never changes). */
    static char s_mac_str[18];
    if (s_mac_str[0] == '\0') {
        uint8_t mac[6] = { 0 };
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK ||
            esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_mac_str, sizeof(s_mac_str), MACSTR, MAC2STR(mac));
        }
    }
    strncpy(out->mac, s_mac_str, sizeof(out->mac) - 1);
    if (s_sta_if) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_if, &ip) == ESP_OK && ip.gw.addr) {
            snprintf(out->sta_gw, sizeof(out->sta_gw), IPSTR, IP2STR(&ip.gw));
        }
    }
}

esp_err_t wifi_mgr_scan_start(void)
{
    if (s_scan_busy) return ESP_ERR_INVALID_STATE;

    /* Scanning needs the STA interface. In AP-only mode (no creds yet, e.g.
     * during captive-portal provisioning) switch to APSTA so the radio can
     * scan while the SoftAP stays up for connected clients. */
    wifi_mode_t cur;
    if (esp_wifi_get_mode(&cur) == ESP_OK && cur == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    s_scan_busy = true;
    s_scan_count = 0;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) s_scan_busy = false;
    return err;
}

bool wifi_mgr_scan_busy(void)
{
    return s_scan_busy;
}

size_t wifi_mgr_scan_results(wifi_mgr_ap_t *out, size_t max)
{
    size_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan_results, n * sizeof(wifi_mgr_ap_t));
    return n;
}

/* Bring the radio into a STA-capable mode and (re)start a connect attempt.
 * Shared by set_sta / connect_saved. Runs on the caller's task (httpd/lvgl). */
static void begin_attempt(void)
{
    wifi_mode_t cur;
    esp_wifi_get_mode(&cur);
    if (cur == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (cur == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }
    esp_wifi_disconnect();
    s_state = WIFI_MGR_STA_CONNECTING;
    s_fast  = true;
    if (s_fallback_timer) {
        xTimerReset(s_fallback_timer, 0);
        xTimerStart(s_fallback_timer, 0);
    }
}

esp_err_t wifi_mgr_set_sta(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    wifi_mgr_add_cred(ssid, psk);            /* persist (best effort if full)   */
    begin_attempt();

    int idx = find_cred(ssid);
    if (idx >= 0) {
        do_connect(idx);
    } else {
        /* List full and SSID is new: connect transiently without persisting. */
        strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
        s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
        apply_sta_cfg(ssid, psk);
        esp_wifi_connect();
    }
    return ESP_OK;
}

int wifi_mgr_get_creds(wifi_cred_t *out, int max)
{
    int n = s_cred_count < max ? s_cred_count : max;
    for (int i = 0; i < n; i++) out[i] = s_creds[i];
    return n;
}

esp_err_t wifi_mgr_add_cred(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    int idx = find_cred(ssid);
    if (idx < 0) {
        if (s_cred_count >= WIFI_MGR_MAX_CREDS) return ESP_ERR_NO_MEM;
        idx = s_cred_count++;
    }
    strncpy(s_creds[idx].ssid, ssid, sizeof(s_creds[idx].ssid) - 1);
    s_creds[idx].ssid[sizeof(s_creds[idx].ssid) - 1] = '\0';
    strncpy(s_creds[idx].psk, psk ? psk : "", sizeof(s_creds[idx].psk) - 1);
    s_creds[idx].psk[sizeof(s_creds[idx].psk) - 1] = '\0';
    save_creds();
    return ESP_OK;
}

esp_err_t wifi_mgr_remove_cred(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    int idx = find_cred(ssid);
    if (idx < 0) return ESP_ERR_NOT_FOUND;

    bool was_active = (strncmp(ssid, s_sta_ssid, sizeof(s_sta_ssid)) == 0);
    for (int i = idx; i < s_cred_count - 1; i++) s_creds[i] = s_creds[i + 1];
    s_cred_count--;
    memset(&s_creds[s_cred_count], 0, sizeof(s_creds[s_cred_count]));
    save_creds();

    if (was_active) {
        esp_wifi_disconnect();
        if (s_cred_count > 0) {
            begin_attempt();
            do_connect(0);
        } else {
            s_sta_ssid[0] = '\0';
            s_sta_ip[0]   = '\0';
            esp_wifi_set_mode(WIFI_MODE_AP);
            start_ap();
            s_state = WIFI_MGR_AP_ONLY;
        }
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_connect_saved(const char *ssid)
{
    int idx = find_cred(ssid);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    begin_attempt();
    do_connect(idx);
    return ESP_OK;
}

void wifi_mgr_get_active_ssid(char *out, size_t outsz)
{
    if (!out || !outsz) return;
    strncpy(out, s_sta_ssid, outsz - 1);
    out[outsz - 1] = '\0';
}

esp_err_t wifi_mgr_clear_sta(void)
{
    nvs_store_clear_wifi_list();
    nvs_store_clear_sta();
    s_cred_count  = 0;
    memset(s_creds, 0, sizeof(s_creds));
    s_sta_ssid[0] = '\0';
    s_sta_ip[0]   = '\0';
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);
    start_ap();
    s_state = WIFI_MGR_AP_ONLY;
    return ESP_OK;
}

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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wifi_mgr";

#define STA_CONNECT_TIMEOUT_MS  20000   /* fall back to AP after this (not associated) */
/* Associated but no IP: the FRITZ!Box answers DHCP in well under a second,
 * but on a weak link (-85 dBm and worse, seen 2026-11-20) the first DISCOVERs
 * get lost and lwIP backs off 2, 4, 8 s. The old code gave the whole connect
 * 20 s from the START of the attempt -- 6 s of that went to the all-channel
 * scan -- and then switched the SoftAP on and moved to the next network,
 * tearing down exactly the association that was about to get its lease. That
 * is how the device stayed offline for minutes after a restart. Now DHCP gets
 * its own timer, started at association, with escalating steps. */
#define DHCP_TIMEOUT_MS         15000
#define DHCP_RETRY_MS           10000   /* after restarting the DHCP client */
/* Last resort: no address on the home network for this long, nobody on the
 * fallback AP -> restart. An offline device is worse than a restart. */
#define NET_WATCHDOG_MS         (15 * 60 * 1000)
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
static TimerHandle_t    s_dhcp_timer;        /* associated, waiting for an address */
static TimerHandle_t    s_netwd_timer;       /* periodic: how long without an address */
static bool             s_assoc;             /* STA associated (CONNECTED event seen) */
static int              s_dhcp_fails;        /* DHCP timeouts on the current network */
static int64_t          s_last_ip_us;        /* last time STA had an address (0 = boot) */
static bool             s_pin_bssid;         /* next connect goes to s_pin (once) */
static uint8_t          s_pin[6];
static volatile bool    s_own_scan;          /* our blocking scan: SCAN_DONE is not the UI's */

/* Mesh: several access points under one name. A node that failed us this
 * session (no DHCP answer, handshake refused) is not picked again; the one
 * that last gave an address is remembered in NVS and tried first at boot. */
#define BAD_MAX 6
typedef struct { uint8_t b[6]; uint8_t n; } strike_t;
static strike_t         s_bad[BAD_MAX];      /* failures per node this session */
static int              s_nbad;
static int              s_mesh_tries;        /* node switches in the current attempt */
static uint8_t          s_last_fail[6];      /* the node that failed just now */
static bool             s_cur_pinned;        /* the running attempt was pinned to a node */
static bool             s_skip_good;         /* next attempt: do not pin the remembered node */
typedef struct { char ssid[33]; uint8_t bssid[6]; uint8_t _rsv; } good_t;
static good_t           s_good;

/* Weak links drop a handshake now and then (2026-11-20: the one node that
 * works timed out once at -85 dBm). One failure is bad luck, two are a
 * pattern; a node that associates but never answers DHCP is out at once. */
static strike_t *strike_of(const uint8_t b[6])
{
    for (int i = 0; i < s_nbad; i++) if (!memcmp(s_bad[i].b, b, 6)) return &s_bad[i];
    return NULL;
}
static bool is_bad(const uint8_t b[6])
{
    strike_t *k = strike_of(b);
    return k && k->n >= 2;
}
static void add_strike(const uint8_t b[6], int n)
{
    static const uint8_t zero[6] = { 0 };
    if (!memcmp(b, zero, 6)) return;
    memcpy(s_last_fail, b, 6);
    strike_t *k = strike_of(b);
    if (!k) {
        k = &s_bad[s_nbad < BAD_MAX ? s_nbad++ : BAD_MAX - 1];
        memcpy(k->b, b, 6);
        k->n = 0;
    }
    bool was = k->n >= 2;
    k->n = (uint8_t)(k->n + n > 9 ? 9 : k->n + n);
    if (!was && k->n >= 2)
        ESP_LOGW(TAG, "node " MACSTR " failed -- not picked again until restart", MAC2STR(b));
}

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
    if (s_pin_bssid) {                  /* one attempt at a chosen mesh node */
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, s_pin, 6);
        s_pin_bssid = false;
    }
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
#define WMGR_NOTIFY_AP_DOWN      (1u << 3)
#define WMGR_NOTIFY_DHCP_TIMEOUT (1u << 4)
#define WMGR_NOTIFY_NET_CHECK    (1u << 5)
#define WMGR_NOTIFY_MESH_NEXT    (1u << 6)
#define WMGR_NOTIFY_SAVE_GOOD    (1u << 7)

static TaskHandle_t s_worker;

/* The fallback SoftAP used to stay up until the next reboot: once STA had lost
 * the router for 20 s the AP came on, and IP_EVENT_STA_GOT_IP never switched
 * back. After every nightly router restart the device therefore advertised an
 * open door -- documented PSK, no password on /ota or the inverter control --
 * to anyone in radio range, for as long as it ran. Now the AP is torn down a
 * grace period after STA has an IP again, unless someone is still using it. */
#define AP_TEARDOWN_GRACE_MS    60000
static TimerHandle_t s_ap_down_timer;

static void ap_down_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_AP_DOWN, eSetBits);
}

static void stop_fallback_ap(void)
{
    if (!s_ap_active || s_state != WIFI_MGR_STA_CONNECTED) return;

    /* Someone provisioning through the captive portal is on the AP right now:
     * kicking them would drop the very page they are looking at. Look again
     * in a minute. */
    wifi_sta_list_t clients = { 0 };
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
        ESP_LOGI(TAG, "fallback AP still has %d client(s) -- keeping it for now", clients.num);
        if (s_ap_down_timer) xTimerStart(s_ap_down_timer, 0);
        return;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP teardown failed: %s", esp_err_to_name(err));
        return;
    }
    s_ap_active = false;
    s_ap_ip[0]  = '\0';
    ESP_LOGI(TAG, "STA is back on %s -- fallback AP torn down", s_sta_ssid);
}

/* Heavy esp_wifi calls (set_config/connect) -- run only from a task with a big
 * stack (worker / app_main / httpd / lvgl), never the event-loop/timer task. */
static void do_connect(int idx);
static void dhcp_timeout(void);
static void net_check(void);
static void mesh_next(void);
static void save_good(void);

static void wifi_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, portMAX_DELAY);

        if (bits & WMGR_NOTIFY_FALLBACK_AP) {
            if (s_state == WIFI_MGR_STA_CONNECTING && s_assoc) {
                /* associated, DHCP still running: the DHCP timer decides */
                ESP_LOGI(TAG, "connect timeout while associated -- leaving it to DHCP");
            } else if (s_state == WIFI_MGR_STA_CONNECTING) {
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
        if (bits & WMGR_NOTIFY_AP_DOWN) {
            stop_fallback_ap();
        }
        if (bits & WMGR_NOTIFY_DHCP_TIMEOUT) {
            dhcp_timeout();
        }
        if (bits & WMGR_NOTIFY_NET_CHECK) {
            net_check();
        }
        if (bits & WMGR_NOTIFY_MESH_NEXT) {
            mesh_next();
        }
        if (bits & WMGR_NOTIFY_SAVE_GOOD) {
            save_good();
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
    if (!s_pin_bssid && !s_skip_good && s_good.ssid[0] && !strcmp(s_good.ssid, s_creds[idx].ssid) &&
        !is_bad(s_good.bssid) && memcmp(s_good.bssid, s_last_fail, 6)) {
        memcpy(s_pin, s_good.bssid, 6);           /* the node that worked last time */
        s_pin_bssid = true;
    }
    s_skip_good  = false;
    s_cur_pinned = s_pin_bssid;
    ESP_LOGI(TAG, "STA try [%d/%d] %s%s", idx + 1, s_cred_count, s_creds[idx].ssid,
             s_pin_bssid ? " (chosen node)" : "");
    if (s_pin_bssid) ESP_LOGI(TAG, "  node " MACSTR, MAC2STR(s_pin));
    apply_sta_cfg(s_creds[idx].ssid, s_creds[idx].psk);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* No connect, no DISCONNECTED event -- and the retry chain is driven
         * by that event alone. Re-arm the slow retry ourselves, or the device
         * would sit in AP fallback forever after one refused call (e.g. an
         * attempt already in flight, or an esp_hosted RPC timeout). */
        ESP_LOGW(TAG, "esp_wifi_connect: %s -- retrying later", esp_err_to_name(err));
        if (s_reconnect_timer) xTimerStart(s_reconnect_timer, 0);
    }
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

static void dhcp_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_DHCP_TIMEOUT, eSetBits);
}

static void netwd_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_NET_CHECK, eSetBits);
}

/* The strongest access point of `ssid` that is not `bad` (a mesh has several
 * under one name). Blocking scan on the worker; false when there is none. */
static bool pick_other_bssid(const char *ssid, uint8_t out[6])
{
    static wifi_ap_record_t recs[SCAN_MAX_APS];    /* ~3 kB: not on the stack */
    wifi_scan_config_t cfg = { .ssid = (uint8_t *)ssid, .show_hidden = false,
                               .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    s_own_scan = true;
    esp_err_t e = esp_wifi_scan_start(&cfg, true);
    uint16_t n = SCAN_MAX_APS;
    if (e == ESP_OK) e = esp_wifi_scan_get_ap_records(&n, recs);
    s_own_scan = false;
    if (e != ESP_OK) { ESP_LOGW(TAG, "mesh scan: %s", esp_err_to_name(e)); return false; }
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp((const char *)recs[i].ssid, ssid) || is_bad(recs[i].bssid) ||
            !memcmp(recs[i].bssid, s_last_fail, 6)) continue;
        bool good = !strcmp(s_good.ssid, ssid) && !memcmp(s_good.bssid, recs[i].bssid, 6);
        bool best_good = best >= 0 && !strcmp(s_good.ssid, ssid) && !memcmp(s_good.bssid, recs[best].bssid, 6);
        /* the node that gave an address last time first, then by signal */
        if (best < 0 || (good && !best_good) || (good == best_good && recs[i].rssi > recs[best].rssi)) best = i;
    }
    ESP_LOGI(TAG, "mesh scan: %u AP(s) named %s, %s", n, ssid, best < 0 ? "no other node" : "trying another node");
    if (best < 0) return false;
    memcpy(out, recs[best].bssid, 6);
    ESP_LOGI(TAG, "  -> " MACSTR " ch %d RSSI %d", MAC2STR(out), recs[best].primary, recs[best].rssi);
    return true;
}

/* A node refused the handshake: the next node of the same network, if there
 * is one we have not tried; otherwise on as before (next network / slow). */
static void mesh_next(void)
{
    if (s_state != WIFI_MGR_STA_CONNECTING && s_state != WIFI_MGR_AP_FALLBACK) return;
    if (s_mesh_tries < BAD_MAX && pick_other_bssid(s_sta_ssid, s_pin)) {
        s_mesh_tries++;
        s_pin_bssid = true;
        do_connect(s_try_idx);
        return;
    }
    s_mesh_tries = 0;
    if (s_state == WIFI_MGR_STA_CONNECTING && s_fast && s_try_idx + 1 < s_cred_count) {
        do_connect(s_try_idx + 1);
        return;
    }
    s_fast = false;
    if (s_reconnect_timer) xTimerStart(s_reconnect_timer, 0);
}

static void save_good(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (!strcmp(s_good.ssid, s_sta_ssid) && !memcmp(s_good.bssid, ap.bssid, 6)) return;
    memset(&s_good, 0, sizeof(s_good));
    strncpy(s_good.ssid, s_sta_ssid, sizeof(s_good.ssid) - 1);
    memcpy(s_good.bssid, ap.bssid, 6);
    nvs_store_set_wifi_good(&s_good, sizeof(s_good));
    ESP_LOGI(TAG, "remembered node " MACSTR " of %s for the next start", MAC2STR(ap.bssid), s_good.ssid);
}

/* Associated for DHCP_TIMEOUT_MS without an address. Step by step, cheapest
 * first: (1) restart the DHCP client, (2) re-associate with the same network,
 * (3) give up on it like on a failed connect -- next network, SoftAP. */
static void dhcp_timeout(void)
{
    if (!s_assoc || s_sta_ip[0]) return;               /* got one meanwhile */
    s_dhcp_fails++;
    wifi_ap_record_t ap = { 0 };
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    ESP_LOGW(TAG, "associated with %s (" MACSTR ", ch %d, RSSI %d) but no address -- step %d",
             s_sta_ssid, MAC2STR(ap.bssid), ap.primary, rssi, s_dhcp_fails);
    if (s_dhcp_fails == 1) {
        esp_netif_dhcpc_stop(s_sta_if);
        esp_err_t e = esp_netif_dhcpc_start(s_sta_if);
        if (e != ESP_OK) ESP_LOGW(TAG, "dhcpc restart: %s", esp_err_to_name(e));
        xTimerChangePeriod(s_dhcp_timer, pdMS_TO_TICKS(DHCP_RETRY_MS), 0);   /* also starts it */
        return;
    }
    s_assoc = false;
    esp_wifi_disconnect();              /* ASSOC_LEAVE: the event handler ignores it */
    if (s_dhcp_fails == 2) {
        /* Same network once more -- at ANOTHER mesh node if there is one: on
         * 2026-11-20 one FRITZ! node (ch 10, -86 dBm) associated the display
         * but never gave it an address, the neighbouring node answered DHCP
         * in 1.5 s. With both at about the same level it was luck which one
         * a restart picked. */
        add_strike(ap.bssid, 2);
        s_mesh_tries = 1;
        if (pick_other_bssid(s_sta_ssid, s_pin)) s_pin_bssid = true;
        do_connect(s_try_idx);
        return;
    }
    s_dhcp_fails = 0;
    if (s_state == WIFI_MGR_STA_CONNECTING && s_fast && s_try_idx + 1 < s_cred_count) {
        do_connect(s_try_idx + 1);
        return;
    }
    s_fast = false;
    if (s_state == WIFI_MGR_STA_CONNECTING && !s_ap_active) {
        ESP_LOGW(TAG, "no address on any network -- falling back to AP");
        s_state = WIFI_MGR_AP_FALLBACK;
        start_ap();
    }
    if (s_reconnect_timer) xTimerStart(s_reconnect_timer, 0);
}

/* Every minute: how long has the device been without an address on the home
 * network? After NET_WATCHDOG_MS restart -- unless someone is on the SoftAP
 * (setting up WiFi through the captive portal) or there is nothing to join. */
static void net_check(void)
{
    if (s_sta_ip[0] || s_cred_count == 0) return;
    int64_t since = s_last_ip_us;
    int64_t off_ms = (esp_timer_get_time() - since) / 1000;
    if (off_ms < NET_WATCHDOG_MS) return;
    if (s_ap_active) {
        wifi_sta_list_t clients = { 0 };
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) return;
    }
    ESP_LOGE(TAG, "no address on the home network for %lld min -- restarting", (long long)(off_ms / 60000));
    esp_restart();
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
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = data;
            s_assoc = true;
            if (e) ESP_LOGI(TAG, "associated with %.*s, BSSID " MACSTR ", ch %d",
                            e->ssid_len, (const char *)e->ssid, MAC2STR(e->bssid), e->channel);
            if (s_dhcp_timer) xTimerChangePeriod(s_dhcp_timer, pdMS_TO_TICKS(DHCP_TIMEOUT_MS), 0);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            /* ASSOC_LEAVE is the reason the station itself puts on the wire
             * when WE call esp_wifi_disconnect() -- begin_attempt() does that
             * before every user-initiated connect. Treating it as a failed
             * attempt advanced s_try_idx to the NEXT saved network: tap network
             * B in the list, end up on C. Nobody else ever produces this code
             * (an AP that throws us out uses its own reasons), so drop it. */
            wifi_event_sta_disconnected_t *e = data;
            s_assoc = false;
            if (s_dhcp_timer) xTimerStop(s_dhcp_timer, 0);
            if (e) ESP_LOGW(TAG, "STA disconnected from %.*s, reason %d, RSSI %d",
                            e->ssid_len, (const char *)e->ssid, e->reason, e->rssi);
            if (e && e->reason == WIFI_REASON_ASSOC_LEAVE) {
                s_sta_ip[0] = '\0';
                s_rssi = 0;
                break;
            }
            s_sta_ip[0] = '\0';
            s_rssi = 0;
            /* A node that refuses the handshake or drops us during it: try the
             * other nodes of the same network before anything else. 201 (no
             * AP found) has no node to blame. */
            if (s_state != WIFI_MGR_STA_CONNECTED && e &&
                (e->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || e->reason == WIFI_REASON_AUTH_EXPIRE ||
                 e->reason == WIFI_REASON_HANDSHAKE_TIMEOUT || e->reason == WIFI_REASON_ASSOC_EXPIRE ||
                 e->reason == WIFI_REASON_CONNECTION_FAIL || e->reason == WIFI_REASON_BEACON_TIMEOUT)) {
                add_strike(e->bssid, 1);
                if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_MESH_NEXT, eSetBits);
                break;
            }
            /* A pinned node that the scan did not see (weak, missed once):
             * the same network again at once, free choice of node. */
            if (s_state != WIFI_MGR_STA_CONNECTED && e && s_cur_pinned &&
                e->reason == WIFI_REASON_NO_AP_FOUND) {
                s_cur_pinned = false;
                s_skip_good  = true;
                request_connect(s_try_idx);
                break;
            }
            if (s_state == WIFI_MGR_STA_CONNECTED) {
                s_last_ip_us = esp_timer_get_time();       /* offline from now */
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
        }
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = SCAN_MAX_APS;
            /* static, NOT on the stack: this handler runs on the small
             * sys_evt task stack (~2.3 KB) and wifi_ap_record_t[32] is
             * ~3 KB -- a stack array here overflows it and panics. */
            static wifi_ap_record_t recs[SCAN_MAX_APS];
            if (s_own_scan) break;          /* pick_other_bssid() reads its own result */
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
            s_dhcp_fails = 0;
            s_mesh_tries = 0;
            memset(s_last_fail, 0, sizeof(s_last_fail));
            s_last_ip_us = esp_timer_get_time();
            if (s_worker) xTaskNotify(s_worker, WMGR_NOTIFY_SAVE_GOOD, eSetBits);
            if (s_dhcp_timer) xTimerStop(s_dhcp_timer, 0);
            if (s_fallback_timer) xTimerStop(s_fallback_timer, 0);
            if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
            ESP_LOGI(TAG, "STA got IP %s on %s, RSSI %d", s_sta_ip, s_sta_ssid, s_rssi);
            /* Router is back: give the fallback AP a grace period, then drop it
             * (see stop_fallback_ap). Runs on the worker, not here. */
            if (s_ap_active && s_ap_down_timer) {
                xTimerReset(s_ap_down_timer, 0);
                xTimerStart(s_ap_down_timer, 0);
            }
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
    s_dhcp_timer      = xTimerCreate("wifi_dhcp",
                                    pdMS_TO_TICKS(DHCP_TIMEOUT_MS),
                                    pdFALSE, NULL, dhcp_timer_cb);
    s_netwd_timer     = xTimerCreate("wifi_wd", pdMS_TO_TICKS(60000),
                                    pdTRUE, NULL, netwd_timer_cb);
    if (s_netwd_timer) xTimerStart(s_netwd_timer, 0);
    s_ap_down_timer   = xTimerCreate("wifi_apdn",
                                    pdMS_TO_TICKS(AP_TEARDOWN_GRACE_MS),
                                    pdFALSE, NULL, ap_down_timer_cb);

    /* Worker that runs the stack-heavy WiFi actions off the timer task. */
    xTaskCreate(wifi_worker_task, "wifi_mgr_wk", 5120, NULL, 5, &s_worker);

    load_creds();
    if (nvs_store_get_wifi_good(&s_good, sizeof(s_good)) == ESP_OK) {
        s_good.ssid[sizeof(s_good.ssid) - 1] = '\0';
        ESP_LOGI(TAG, "last good node: " MACSTR " of %s", MAC2STR(s_good.bssid), s_good.ssid);
    } else {
        memset(&s_good, 0, sizeof(s_good));
    }
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
    /* State first, then disconnect: the DISCONNECTED event is asynchronous and
     * must not find us still in CONNECTED (it would "re-select from the top"). */
    s_state = WIFI_MGR_STA_CONNECTING;
    s_fast  = true;
    if (s_ap_down_timer) xTimerStop(s_ap_down_timer, 0);
    esp_wifi_disconnect();
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

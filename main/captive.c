#include "captive.h"
#include "wifi_mgr.h"
#include "web_mirror.h"
#include "ota.h"
#include "deye_web.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "captive";

/* The web UI ("/") is the live display mirror, served by web_mirror.c. This
 * module provides the DNS hijack + captive redirect + WiFi scan/connect JSON
 * endpoints around it. */

#define DNS_PORT          53
#define DNS_BUF_LEN       256
#define SCAN_WAIT_MAX_MS  6000
#define SCAN_MAX_APS      32

static httpd_handle_t s_httpd;
static TaskHandle_t   s_dns_task;
static int            s_dns_sock = -1;
static volatile bool  s_dns_run;

/* ------------------------------ helpers -------------------------------- */

static void ap_ip_str(char *out, size_t sz)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(out, sz, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, sz, "192.168.4.1");
    }
}

static uint32_t ap_ip_u32(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr) {
        return ip.ip.addr;            /* already network byte order */
    }
    return ipaddr_addr("192.168.4.1");
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* application/x-www-form-urlencoded decode ('+' -> space, %XX -> byte). */
static void url_decode(char *dst, size_t dsz, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dsz; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            int hi = hexval(src[i + 1]);
            int lo = hi >= 0 ? hexval(src[i + 2]) : -1;
            if (lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
}

/* Minimal JSON string escaping (quotes/backslashes; drop control bytes). */
static void json_escape(char *dst, size_t dsz, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 2 < dsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c >= 0x20) {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

/* ------------------------------ DNS hijack ----------------------------- */

/* Bind the hijack socket. Returns false and leaves s_dns_sock at -1 on failure
 * (the caller retries later -- a failure here is usually the shared lwIP socket
 * pool being momentarily empty, not a permanent condition). */
static bool dns_open(void)
{
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed (errno %d)", errno);
        return false;
    }

    struct sockaddr_in srv = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed (errno %d)", errno);
        close(s_dns_sock);
        s_dns_sock = -1;
        return false;
    }

    /* 1 s receive timeout: doubles as the polling cadence for "is the AP still
     * up?", so the socket is released within a second of the AP going away. */
    struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    ESP_LOGI(TAG, "DNS hijack listening on :%d", DNS_PORT);
    return true;
}

static void dns_close(void)
{
    if (s_dns_sock < 0) return;
    close(s_dns_sock);
    s_dns_sock = -1;
    ESP_LOGI(TAG, "DNS hijack stopped (no AP) -- socket released");
}

/* Answer every A query with the AP IP so the OS captive-portal probe is
 * redirected to our page.
 *
 * The socket is held ONLY while the SoftAP is actually up. It used to be bound
 * for the entire uptime, which permanently spent one of the 16 lwIP sockets
 * shared with both HTTP servers, MQTT, WireGuard, the Modbus-TCP pollers and
 * the Modbus bridge -- on a device that is in plain STA mode 99.9% of the time
 * and where a dry pool makes port 502 refuse connections. In STA mode the
 * hijack has no job anyway: nobody is connected to an AP that isn't running. */
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_BUF_LEN];
    uint8_t tx[DNS_BUF_LEN + 16];

    while (s_dns_run) {
        wifi_mgr_status_t ws;
        wifi_mgr_get_status(&ws);

        if (!ws.ap_active) {
            dns_close();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_dns_sock < 0 && !dns_open()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s_dns_sock, rx, sizeof(rx), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) {
            continue;                 /* timeout or runt packet */
        }
        if (n > DNS_BUF_LEN) {
            n = DNS_BUF_LEN;
        }

        memcpy(tx, rx, n);
        tx[2] = 0x81;                 /* QR=1, RD copied */
        tx[3] = 0x80;                 /* RA=1, RCODE=0   */
        tx[6] = 0x00; tx[7] = 0x01;   /* ANCOUNT = 1     */
        tx[8] = 0x00; tx[9] = 0x00;   /* NSCOUNT = 0     */
        tx[10] = 0x00; tx[11] = 0x00; /* ARCOUNT = 0     */

        int p = n;
        tx[p++] = 0xC0; tx[p++] = 0x0C;          /* name -> offset 12   */
        tx[p++] = 0x00; tx[p++] = 0x01;          /* type A              */
        tx[p++] = 0x00; tx[p++] = 0x01;          /* class IN            */
        tx[p++] = 0x00; tx[p++] = 0x00;
        tx[p++] = 0x00; tx[p++] = 0x3C;          /* TTL 60s             */
        tx[p++] = 0x00; tx[p++] = 0x04;          /* RDLENGTH 4          */
        uint32_t ip = ap_ip_u32();
        memcpy(&tx[p], &ip, 4);                  /* RDATA = AP IP       */
        p += 4;

        sendto(s_dns_sock, tx, p, 0, (struct sockaddr *)&src, slen);
    }

    dns_close();
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------ HTTP handlers -------------------------- */

/* Trigger a scan, wait for it, return the results as a JSON array. */
static esp_err_t h_scan(httpd_req_t *req)
{
    /* single httpd worker task -> these statics are not re-entered */
    static wifi_mgr_ap_t aps[SCAN_MAX_APS];
    static char json[4096];

    wifi_mgr_scan_start();
    int waited = 0;
    while (wifi_mgr_scan_busy() && waited < SCAN_WAIT_MAX_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }

    size_t n = wifi_mgr_scan_results(aps, SCAN_MAX_APS);

    size_t len = 0, added = 0;
    len += snprintf(json + len, sizeof(json) - len, "[");
    for (size_t i = 0; i < n; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue;                 /* hidden / empty SSID */
        }
        char esc[80];
        json_escape(esc, sizeof(esc), aps[i].ssid);
        len += snprintf(json + len, sizeof(json) - len,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                        added ? "," : "", esc, aps[i].rssi,
                        aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        added++;
        if (len >= sizeof(json) - 96) {
            break;
        }
    }
    snprintf(json + len, sizeof(json) - len, "]");

    ESP_LOGI(TAG, "portal scan -> %u networks", (unsigned)added);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t h_connect(httpd_req_t *req)
{
    char body[256];
    int total = req->content_len < (int)sizeof(body) - 1
                    ? req->content_len : (int)sizeof(body) - 1;
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        off += r;
    }
    body[off] = '\0';

    char ssid_enc[160] = { 0 }, psk_enc[160] = { 0 };
    httpd_query_key_value(body, "ssid", ssid_enc, sizeof(ssid_enc));
    httpd_query_key_value(body, "psk",  psk_enc,  sizeof(psk_enc));

    char ssid[33] = { 0 }, psk[65] = { 0 };
    url_decode(ssid, sizeof(ssid), ssid_enc);
    url_decode(psk,  sizeof(psk),  psk_enc);

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Missing SSID");
    }

    ESP_LOGI(TAG, "portal connect request -> SSID '%s'", ssid);
    esp_err_t err = wifi_mgr_set_sta(ssid, psk);

    char page[512];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<style>body{font-family:system-ui,sans-serif;background:#0b0d10;"
             "color:#e5e7eb;padding:24px;text-align:center}h1{font-size:20px}"
             "p{color:#9aa0a8}</style></head><body>"
             "<h1>%s</h1><p>Das Display verbindet sich mit <b>%s</b> und "
             "verl&auml;sst gleich diesen Hotspot. Du kannst dein Handy wieder "
             "mit dem Heimnetz verbinden.</p></body></html>",
             err == ESP_OK ? "Verbinde &hellip;" : "Fehler beim Speichern",
             ssid);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, page);
}

/* Catch-all: redirect any other request to the portal so the OS connectivity
 * check fails and the captive-portal sheet opens. */
static esp_err_t h_redirect(httpd_req_t *req)
{
    char ip[16];
    ap_ip_str(ip, sizeof(ip));
    char loc[32];
    snprintf(loc, sizeof(loc), "http://%s/", ip);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------ public API ----------------------------- */

esp_err_t captive_start(void)
{
    if (s_httpd) {
        return ESP_OK;                /* already running */
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 6144;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 24;   /* mirror + ota + deye + captive routes */
    cfg.recv_wait_timeout = 12;  /* grace for a large OTA upload under load */
    cfg.send_wait_timeout = 12;
    /* 4, not 7. These are not free slots, they are 4 of the 16 lwIP sockets the
     * whole device shares -- and this server plus the :81 stream server plus the
     * DNS hijack used to reserve 14 of them, leaving the Modbus bridge and the
     * Modbus-TCP pollers to fight over the rest. accept() then failed with
     * ENFILE and port 502 refused LAN clients. 4 still covers the page + its
     * /api/live poll + an OTA upload alongside; lru_purge_enable recycles the
     * oldest connection instead of failing when a browser opens more. */
    cfg.max_open_sockets = 4;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd start");

    /* Order matters: exact routes first, the "/*" catch-all redirect LAST,
     * or the wildcard would swallow every request. The mirror owns "/" and
     * "/touch". */
    web_mirror_register(s_httpd);
    ota_register_routes(s_httpd);
    deye_web_register(s_httpd);

    const httpd_uri_t scan = { .uri = "/scan",    .method = HTTP_GET,  .handler = h_scan };
    const httpd_uri_t conn = { .uri = "/connect", .method = HTTP_POST, .handler = h_connect };
    const httpd_uri_t redir = { .uri = "/*",      .method = HTTP_GET,  .handler = h_redirect };
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &conn);
    httpd_register_uri_handler(s_httpd, &redir);

    s_dns_run = true;
    if (xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, &s_dns_task)
            != pdPASS) {
        ESP_LOGE(TAG, "DNS task create failed");
        s_dns_run = false;
    }

    /* Not "+ DNS :53" any more: the hijack task is up but binds nothing until
     * the SoftAP actually runs, so claiming the port here would be a lie in the
     * common (STA) case. */
    ESP_LOGI(TAG, "captive portal up (HTTP :80; DNS :53 follows the SoftAP)");
    return ESP_OK;
}

void captive_stop(void)
{
    /* The DNS task now opens and closes its own socket (it only holds one while
     * the AP is up), so closing it from here would race a reopen and could shut
     * down a descriptor another module has since been handed. Signal instead and
     * give it up to ~2 s to fall out of its 1 s recvfrom and clean up itself. */
    s_dns_run = false;
    for (int i = 0; i < 40 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

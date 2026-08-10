#include "modbus_gw.h"
#include "modbus_rtu.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "mb_gw";

/* Modbus-TCP framing: 7-byte MBAP header + PDU.
 *   [txid hi][txid lo][proto hi][proto lo][len hi][len lo][unit][PDU...]
 * `len` counts the unit byte plus the PDU. */
#define GW_MBAP      7
#define GW_UNIT      6            /* offset of the unit-id byte in the header */
#define GW_PDU_MAX   253
#define GW_FRAME_MAX (GW_MBAP + GW_PDU_MAX)
/* One full frame is enough: a pipelined remainder simply stays in the lwIP
 * receive queue and is picked up by the next select() round, which is already
 * readable. Sizing this 512 would cost another 2 KB of internal SRAM across the
 * client slots for no gain. */
#define GW_RXBUF     GW_FRAME_MAX

/* gw_serve() forwards the request straight out of the receive buffer and lets
 * the RTU answer land straight in the reply frame -- no repacking. That makes
 * these size relations load-bearing in BOTH directions, and a violation would
 * be a silent buffer overrun rather than a compile error. */
_Static_assert(1 + GW_PDU_MAX + 2 <= MB_RTU_ADU_MAX,
               "[unit][PDU] + CRC must fit an RTU ADU");
_Static_assert(MB_RTU_ADU_MAX - 2 <= GW_FRAME_MAX - GW_UNIT,
               "an RTU response without CRC must fit the MBAP reply frame");
_Static_assert(GW_RXBUF >= GW_FRAME_MAX,
               "the receive buffer must hold one maximum-size frame");

/* Idle connections are reaped so a crashed client cannot hold a slot forever.
 * 60 s, not the 300 s this started at: with only gw_max_clients slots (2 by
 * default) a client that dies without a FIN -- HA restarting, a WiFi dropout,
 * a laptop closing its lid -- parks its slot for the whole window, and the
 * reconnect is then refused. Five minutes of that reads as "the Deye is not
 * reachable over Modbus". A healthy poller sends far more often than once a
 * minute, so nothing legitimate is dropped; TCP keepalive below catches the
 * half-open case even sooner. */
#define GW_IDLE_MS   60000

/* TCP keepalive on accepted clients: probe an idle peer after 20 s, then every
 * 5 s, and drop it after 3 unanswered probes (~35 s). This is what actually
 * frees a slot whose peer vanished silently -- the idle reaper above only
 * covers a peer that is alive but quiet. */
#define GW_KA_IDLE_S   20
#define GW_KA_INTVL_S   5
#define GW_KA_COUNT     3

/* Below LVGL/touch (priority 4), like the Modbus-TCP poll workers -- a busy
 * bridge must never make the UI sluggish. Pinned to core 0 for the same reason
 * (LVGL owns core 1). The task spends nearly all its time blocked in select()
 * or waiting on the RS485 bus, so it costs almost nothing. */
#define GW_PRIO      3
/* Measured with -fstack-usage: gw_task 96 + gw_recv 32 + gw_serve 320 +
 * modbus_rtu_txn 48 = 496 B of own frames, leaving ~3 KB for the lwIP and
 * ESP_LOG tail -- more headroom than modbus_tcp.c's worker_task has proven at
 * with 1104 B of frames in 4096. */
#define GW_STACK     3584

typedef struct {
    int      fd;
    int      have;                /* bytes buffered from a partial frame */
    uint32_t last_ms;
    uint8_t  buf[GW_RXBUF];
} gw_client_t;

static gw_client_t        s_cl[MB_GW_MAX_CLIENTS];
static modbus_gw_status_t s_st;
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ----------------------------- socket helpers -------------------------- */

static void gw_close(gw_client_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd   = -1;
    c->have = 0;
}

static int send_all(int fd, const uint8_t *p, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = send(fd, p + sent, n - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

static int gw_listen(uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        /* Same shared-pool failure as in gw_accept(), just one step earlier:
         * without a listener the bridge never comes up at all. */
        ESP_LOGE(TAG, "socket for :%u failed (errno %d) -- bridge not listening",
                 port, errno);
        return -1;
    }
    int one = 1;
    /* Without SO_REUSEADDR a re-bind after a config change fails for as long as
     * the old socket lingers in TIME_WAIT. */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind :%u failed (errno %d)", port, errno);
        close(s);
        return -1;
    }
    if (listen(s, MB_GW_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "listen :%u failed (errno %d)", port, errno);
        close(s);
        return -1;
    }
    return s;
}

static void gw_shutdown(int *lsock)
{
    for (int i = 0; i < MB_GW_MAX_CLIENTS; i++) gw_close(&s_cl[i]);
    if (*lsock >= 0) { close(*lsock); *lsock = -1; }
}

/* ----------------------------- request path ---------------------------- */

/* Forward one complete MBAP frame to the RS485 side and answer the client. */
static void gw_serve(gw_client_t *c, const uint8_t *f, int flen, uint32_t tmo_ms)
{
    uint8_t unit    = f[GW_UNIT];
    uint8_t fc      = f[GW_MBAP];
    int     pdu_len = flen - GW_MBAP;              /* >= 1, checked by caller */

    /* No repacking: an MBAP frame from GW_UNIT on is [unit][PDU], and an RTU
     * ADU without its CRC is [slave id][PDU] -- byte-identical layouts. So the
     * request goes to the bus straight out of the receive buffer and the answer
     * is received straight into the reply frame, saving 512 B of stack and two
     * memcpys per request. */
    uint8_t out[GW_FRAME_MAX];
    int bus = modbus_rtu_gw_route(unit);
    int n   = (bus < 0) ? -1
            : modbus_rtu_txn(bus, f + GW_UNIT, pdu_len + 1,
                             out + GW_UNIT, GW_FRAME_MAX - GW_UNIT, tmo_ms);

    int body;
    if (n > 1) {
        /* A Modbus EXCEPTION from the device is a valid answer and is relayed
         * untouched -- the inverter refusing a register is the client's
         * business, not a bridge failure. */
        body = n - 1;                              /* minus the echoed slave id */
        portENTER_CRITICAL(&s_mux); s_st.req_ok++; portEXIT_CRITICAL(&s_mux);
    } else {
        /* Standard gateway exceptions, so the client can tell "no bus serves
         * this unit id" (0x0A) from "the RS485 device stayed silent" (0x0B). */
        out[GW_MBAP]     = (uint8_t)(fc | 0x80);
        out[GW_MBAP + 1] = (bus < 0) ? 0x0A : 0x0B;
        body = 2;
        portENTER_CRITICAL(&s_mux); s_st.req_err++; portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "unit %u fc 0x%02X -> %s (rc %d)", unit, fc,
                 bus < 0 ? "no bridged bus" : "no RTU reply", n);
    }

    uint16_t len = (uint16_t)(body + 1);           /* unit byte + PDU */
    out[0] = f[0]; out[1] = f[1];                  /* echo the transaction id */
    out[2] = 0; out[3] = 0;                        /* protocol id = Modbus */
    out[4] = (uint8_t)(len >> 8); out[5] = (uint8_t)len;
    out[GW_UNIT] = unit;                           /* == the id the RTU echoed */
    if (send_all(c->fd, out, GW_MBAP + body) < 0) gw_close(c);
}

/* Drain the socket and dispatch every COMPLETE frame it yielded. TCP is a
 * stream: a read can carry half a request or several pipelined ones. */
static void gw_recv(gw_client_t *c, uint32_t now, uint32_t tmo_ms)
{
    /* Space is always > 0: the loop below cannot leave a COMPLETE frame in the
     * buffer, and the static assert above guarantees one frame fits. */
    int r = recv(c->fd, c->buf + c->have, (int)sizeof(c->buf) - c->have, 0);
    if (r <= 0) { gw_close(c); return; }           /* peer closed or errored   */
    c->have   += r;
    c->last_ms = now;

    while (c->have >= GW_MBAP) {
        uint16_t proto = (uint16_t)((c->buf[2] << 8) | c->buf[3]);
        uint16_t len   = (uint16_t)((c->buf[4] << 8) | c->buf[5]);
        /* A bad header means the stream is out of sync; there is no resync
         * marker in Modbus-TCP, so the only correct move is to drop the peer. */
        if (proto != 0 || len < 2 || len > GW_PDU_MAX + 1) {
            ESP_LOGW(TAG, "bad MBAP (proto %u, len %u) -- closing client", proto, len);
            gw_close(c);
            return;
        }
        int total = 6 + len;
        if (c->have < total) break;                /* frame still incomplete   */

        gw_serve(c, c->buf, total, tmo_ms);
        if (c->fd < 0) return;                     /* send failed -> closed    */

        c->have -= total;
        if (c->have > 0) memmove(c->buf, c->buf + total, c->have);
    }
}

static void gw_accept(int lsock, int maxcl, uint32_t now)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = accept(lsock, (struct sockaddr *)&sa, &sl);
    if (fd < 0) {
        /* This used to `return` silently, and that silence was the whole
         * problem: the 16 lwIP sockets are shared with both HTTP servers, MQTT,
         * WireGuard and the PERSISTENT Modbus-TCP worker sockets, so accept()
         * here fails with ENFILE (23) whenever the pool runs dry. The bridge
         * then refuses every LAN client while its status line still reads
         * "running" and req_err stays at 0 -- the failure was invisible both on
         * the display and in the log. Count it and say so. */
        int e = errno;
        portENTER_CRITICAL(&s_mux);
        s_st.conn_rej++; s_st.last_errno = e;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "accept failed (errno %d%s) -- LAN client refused", e,
                 e == ENFILE || e == EMFILE ? ": lwIP socket pool exhausted" : "");
        /* Do not spin: with the pool empty the next accept() fails just as
         * fast, and a client retrying every 1.5 s would flood the log. */
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
    }

    int slot = -1, used = 0;
    for (int i = 0; i < MB_GW_MAX_CLIENTS; i++) {
        if (s_cl[i].fd >= 0) used++;
        else if (slot < 0)   slot = i;
    }
    if (slot < 0 || used >= maxcl) {
        /* Close immediately instead of leaving it parked in the backlog: the
         * client gets a clean error and can retry, rather than hanging. */
        close(fd);
        portENTER_CRITICAL(&s_mux);
        s_st.conn_rej++;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "client limit %d reached -- refused %s",
                 maxcl, inet_ntoa(sa.sin_addr));
        return;
    }

    int one = 1;
    /* Modbus frames are tiny; Nagle would add up to 40 ms per response. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* Bound a blocked send so one wedged client cannot stall the whole task. */
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    /* Keepalive: reclaim the slot of a peer that vanished without a FIN. A
     * client slot is a scarce resource here (2 by default), and a half-open
     * connection is otherwise indistinguishable from an idle healthy one. */
    int ka_idle = GW_KA_IDLE_S, ka_intvl = GW_KA_INTVL_S, ka_cnt = GW_KA_COUNT;
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &one,      sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle,  sizeof(ka_idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,   sizeof(ka_cnt));

    s_cl[slot].fd      = fd;
    s_cl[slot].have    = 0;
    s_cl[slot].last_ms = now;
    ESP_LOGI(TAG, "client %s connected (slot %d, %d/%d)",
             inet_ntoa(sa.sin_addr), slot, used + 1, maxcl);
}

/* ------------------------------- task ---------------------------------- */

static void gw_task(void *arg)
{
    (void)arg;
    int      lsock = -1;
    uint16_t bound = 0;

    for (;;) {
        /* clamp_cfg() in modbus_rtu.c is the single authority for the defaults
         * and the ceilings; it runs on both the load and the save path, so
         * nothing here can be 0 or out of range. */
        mb_rtu_cfg_t cfg;
        modbus_rtu_get_cfg(&cfg);
        bool     want  = cfg.gw_enabled != 0;
        uint16_t port  = cfg.gw_port;
        uint32_t tmo   = cfg.gw_timeout_ms;
        int      maxcl = cfg.gw_max_clients;

        /* Switched off or moved to another port -> tear the listener down. */
        if (lsock >= 0 && (!want || port != bound)) {
            ESP_LOGI(TAG, "bridge %s", want ? "rebinding" : "stopped");
            gw_shutdown(&lsock);
        }
        if (want && lsock < 0) {
            lsock = gw_listen(port);
            if (lsock >= 0) {
                bound = port;
                ESP_LOGI(TAG, "Modbus-TCP bridge listening on :%u (max %d clients)",
                         port, maxcl);
            }
        }

        int clients = 0;
        for (int i = 0; i < MB_GW_MAX_CLIENTS; i++) if (s_cl[i].fd >= 0) clients++;
        portENTER_CRITICAL(&s_mux);
        s_st.running = (lsock >= 0);
        s_st.clients = (uint8_t)clients;
        portEXIT_CRITICAL(&s_mux);

        if (lsock < 0) {
            /* Off, or the bind failed (WiFi not up yet) -- retry unobtrusively. */
            vTaskDelay(pdMS_TO_TICKS(want ? 2000 : 500));
            continue;
        }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lsock, &rd);
        int mx = lsock;
        for (int i = 0; i < MB_GW_MAX_CLIENTS; i++) {
            if (s_cl[i].fd < 0) continue;
            FD_SET(s_cl[i].fd, &rd);
            if (s_cl[i].fd > mx) mx = s_cl[i].fd;
        }

        /* 500 ms tick doubles as the config-change and idle-reaper cadence. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
        int r = select(mx + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
            ESP_LOGW(TAG, "select failed (errno %d) -- restarting listener", errno);
            gw_shutdown(&lsock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = now_ms();
        if (r > 0 && FD_ISSET(lsock, &rd)) gw_accept(lsock, maxcl, now);

        for (int i = 0; i < MB_GW_MAX_CLIENTS; i++) {
            if (s_cl[i].fd < 0) continue;
            if (r > 0 && FD_ISSET(s_cl[i].fd, &rd)) {
                gw_recv(&s_cl[i], now, tmo);
            } else if ((uint32_t)(now - s_cl[i].last_ms) > GW_IDLE_MS) {
                ESP_LOGI(TAG, "client slot %d idle -- closed", i);
                gw_close(&s_cl[i]);
            }
        }
    }
}

/* ------------------------------ public --------------------------------- */

esp_err_t modbus_gw_start(void)
{
    for (int i = 0; i < MB_GW_MAX_CLIENTS; i++) s_cl[i].fd = -1;
    if (xTaskCreatePinnedToCore(gw_task, "mb_gw", GW_STACK, NULL,
                                GW_PRIO, NULL, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void modbus_gw_get_status(modbus_gw_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

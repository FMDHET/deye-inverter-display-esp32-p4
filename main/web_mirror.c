#include "web_mirror.h"
#include "lvgl_port.h"
#include "ui_settings.h"
#include "board_jc4880p443c.h"
#include "modbus_tcp.h"
#include "mqtt_fwd.h"
#include "ntp_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_mirror";

/* One full frame's worth of pixels. The live logical size depends on the
 * orientation (800x480 landscape or 480x800 portrait) but the pixel COUNT is
 * the same either way, so this sizes every buffer. The actual per-frame width
 * and height are read from the display at grab time. */
#define SCR_W            800
#define SCR_H            480
#define SCR_BYTES        ((size_t)SCR_W * SCR_H * 2)   /* RGB565 */

#define STREAM_PORT      81
#define JPEG_QUALITY     80
#define FRAME_PERIOD_MS  120          /* ~8 fps: caps LVGL-lock contention */
#define MJPEG_BOUNDARY   "deyembjpeg"

/* Page served from web_mirror.html via EMBED_TXTFILES. */
extern const char mirror_html_start[] asm("_binary_web_mirror_html_start");

static jpeg_encoder_handle_t s_enc;
static uint8_t              *s_in;        /* RGB565 frame (landscape), DMA-capable */
static size_t               s_in_cap;
static uint8_t              *s_out;       /* compressed JPEG, DMA-capable */
static size_t               s_out_cap;
static esp_lcd_panel_handle_t s_panel;    /* DPI panel -> its framebuffer is read */
static httpd_handle_t        s_stream_httpd;

/* Web pointer injected into LVGL via a second input device. */
static lv_indev_t           *s_indev;
static volatile bool         s_paused;    /* stream paused (e.g. during OTA) */
static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;
static int                   s_px, s_py;
static bool                  s_pressed;          /* currently held down       */
static bool                  s_release_pending;  /* up arrived, hold 1 sample  */

/* --------------------------- touch injection --------------------------- */

/* LVGL polls this ~every 30 ms. A fast mouse click (down+up between two polls)
 * would otherwise never be sampled as PRESSED, so we latch: once a press is
 * registered we report PRESSED for (at least) one poll before releasing. */
static void web_indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int lx, ly;
    bool pressed;
    portENTER_CRITICAL(&s_mux);
    lx = s_px;
    ly = s_py;
    pressed = s_pressed;
    if (s_pressed && s_release_pending) {
        /* reported the press this cycle -> release on the next one */
        s_pressed = false;
        s_release_pending = false;
    }
    portEXIT_CRITICAL(&s_mux);

    /* The browser sends LOGICAL (post-rotation) coordinates, but LVGL re-applies
     * the active display rotation to every indev point. So report the
     * PRE-rotation (physical 480x800) point whose forward rotation lands back
     * on the real click -- the inverse of the current rotation. */
    const int PW = BOARD_LCD_H_RES;   /* physical width  = 480 */
    const int PH = BOARD_LCD_V_RES;   /* physical height = 800 */
    int rx = lx, ry = ly;
    switch (lv_display_get_rotation(lv_display_get_default())) {
    case LV_DISPLAY_ROTATION_0:   rx = lx;              ry = ly;              break;
    case LV_DISPLAY_ROTATION_90:  rx = ly;              ry = (PH - 1) - lx;   break;
    case LV_DISPLAY_ROTATION_180: rx = (PW - 1) - lx;   ry = (PH - 1) - ly;   break;
    case LV_DISPLAY_ROTATION_270: rx = (PW - 1) - ly;   ry = lx;              break;
    }
    data->point.x = rx;
    data->point.y = ry;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* --------------------------- snapshot + encode ------------------------- */

/* Read the ALREADY-rendered panel framebuffer and rotate it back to the logical
 * landscape orientation, then JPEG it. Crucially this does NOT take the LVGL
 * lock or re-render anything (the old lv_snapshot_take re-ran the full draw
 * pipeline under the lock, starving touch on the busy dashboard). The panel is
 * the physical 480x800 portrait FB; LVGL flushes the logical 800x480 landscape
 * into it rotated 90 deg (270 deg when mounted upside-down), so we apply the
 * inverse rotation here. A little tearing (reading while a flush writes) is fine
 * for a monitoring mirror. */
static esp_err_t grab_frame(uint32_t *out_len)
{
    if (!s_panel) return ESP_FAIL;

    void *fbp = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fbp) != ESP_OK || !fbp)
        return ESP_FAIL;

    const uint16_t *fb  = (const uint16_t *)fbp;   /* 480x800 panel, RGB565 */
    uint16_t       *dst = (uint16_t *)s_in;        /* 800x480 landscape     */
    const int PW = BOARD_LCD_H_RES;                /* panel width  = 480    */
    const int LW = SCR_W, LH = SCR_H;              /* logical 800 x 480     */

    lv_display_rotation_t rot = lv_display_get_rotation(lv_display_get_default());
    if (rot == LV_DISPLAY_ROTATION_270) {          /* upside-down mounting */
        for (int ly = 0; ly < LH; ly++) {
            uint16_t *drow = dst + (size_t)ly * LW;
            for (int lx = 0; lx < LW; lx++)
                drow[lx] = fb[(size_t)lx * PW + (PW - 1 - ly)];
        }
    } else {                                        /* ROTATION_90 (default) */
        for (int ly = 0; ly < LH; ly++) {
            uint16_t *drow = dst + (size_t)ly * LW;
            for (int lx = 0; lx < LW; lx++)
                drow[lx] = fb[(size_t)(LW - 1 - lx) * PW + ly];
        }
    }

    jpeg_encode_cfg_t cfg = {
        .width         = LW,
        .height        = LH,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = JPEG_QUALITY,
    };
    return jpeg_encoder_process(s_enc, &cfg, s_in, (size_t)LW * LH * 2,
                                s_out, s_out_cap, out_len);
}

/* --------------------------- HTTP handlers ----------------------------- */

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, mirror_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t touch_handler(httpd_req_t *req)
{
    char q[64], v[12];
    int  x = 0, y = 0, down = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "x", v, sizeof(v)) == ESP_OK)    x = atoi(v);
        if (httpd_query_key_value(q, "y", v, sizeof(v)) == ESP_OK)    y = atoi(v);
        if (httpd_query_key_value(q, "down", v, sizeof(v)) == ESP_OK) down = atoi(v);
    }
    portENTER_CRITICAL(&s_mux);
    s_px = x;
    s_py = y;
    if (down) {
        s_pressed = true;
        s_release_pending = false;
    } else {
        /* keep s_pressed until the indev has reported it at least once */
        s_release_pending = true;
    }
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "touch x=%d y=%d down=%d", x, y, down);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "ok");
}

/* Physical keyboard from the browser -> focused LVGL text field. */
static esp_err_t key_handler(httpd_req_t *req)
{
    char q[24], v[12];
    long c = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "c", v, sizeof(v)) == ESP_OK) {
        c = atol(v);
    }
    if (c > 0 && app_lvgl_lock(100)) {
        ui_settings_web_key((uint32_t)c);
        app_lvgl_unlock();
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "ok");
}

/* POST /paste : body = UTF-8 clipboard text from the PC -> inserted into the
 * focused text field. Lets you paste long WireGuard keys / IPs from the PC. */
static esp_err_t paste_handler(httpd_req_t *req)
{
    char buf[1025];
    int  stored = 0;
    int  left   = req->content_len;
    char tmp[256];
    while (left > 0) {
        int want = left < (int)sizeof(tmp) ? left : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, want);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        left -= r;
        int space = (int)sizeof(buf) - 1 - stored;   /* keep storing until full, */
        int n = r < space ? r : space;                /* but drain the rest       */
        if (n > 0) { memcpy(buf + stored, tmp, n); stored += n; }
    }
    buf[stored] = '\0';
    if (stored > 0 && app_lvgl_lock(150)) {
        ui_settings_web_paste(buf);
        app_lvgl_unlock();
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "ok");
}

/* GET /copy : returns the focused text field's content so the page can put it
 * on the PC clipboard. (navigator.clipboard write may be blocked on plain HTTP.) */
static esp_err_t copy_handler(httpd_req_t *req)
{
    char buf[1025];
    buf[0] = '\0';
    if (app_lvgl_lock(150)) {
        ui_settings_web_copy(buf, sizeof(buf));
        app_lvgl_unlock();
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, buf);
}

/* Live aggregate values (for remote verification / dashboards). */
static esp_err_t live_handler(httpd_req_t *req)
{
    modbus_tcp_status_t st;
    modbus_tcp_get_status(&st);
    mqtt_cfg_t mc; mqtt_fwd_get_cfg(&mc);
    mqtt_fwd_status_t ms; mqtt_fwd_get_status(&ms);
    char tbuf[24] = "";
    if (ntp_is_synced()) {
        time_t now = time(NULL);
        struct tm tm; localtime_r(&now, &tm);
        snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    lv_display_t *disp = lv_display_get_default();
    int lw = disp ? lv_display_get_horizontal_resolution(disp) : SCR_W;
    int lh = disp ? lv_display_get_vertical_resolution(disp)   : SCR_H;
    char j[420];
    snprintf(j, sizeof(j),
             "{\"dev\":%u,\"conn\":%u,\"polls\":%u,\"err\":%u,"
             "\"pv\":%.0f,\"haus\":%.0f,\"netz\":%.0f,"
             "\"deye_w\":%.0f,\"deye_soc\":%.0f,\"byd_w\":%.0f,\"byd_soc\":%.0f,"
             "\"mqtt_en\":%u,\"mqtt_conn\":%u,\"mqtt_host\":\"%s\","
             "\"ntp\":%u,\"time\":\"%s\",\"w\":%d,\"h\":%d}",
             st.dev_count, st.connected, (unsigned)st.poll_count, (unsigned)st.err_count,
             st.pv_w, st.house_w, st.grid_w,
             st.deye_w, st.deye_soc, st.byd_w, st.byd_soc,
             mc.enabled ? 1u : 0u, ms.connected ? 1u : 0u, mc.host,
             ntp_is_synced() ? 1u : 0u, tbuf, lw, lh);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, j);
}

/* Per-device live values (debug / drill-down). */
static esp_err_t devices_handler(httpd_req_t *req)
{
    mb_dev_live_t dl[MB_MAX_DEVICES];
    int n = modbus_tcp_get_device_live(dl, MB_MAX_DEVICES);
    char j[1280];
    size_t o = 0;
    o += snprintf(j + o, sizeof(j) - o, "[");
    for (int i = 0; i < n; i++) {
        o += snprintf(j + o, sizeof(j) - o,
                      "%s{\"name\":\"%s\",\"ip\":\"%s\",\"slave\":%u,\"mfr\":\"%s\",\"role\":\"%s\",\"conn\":%d,"
                      "\"pv\":%.0f,\"w\":%.0f,\"soc\":%.0f}",
                      i ? "," : "", dl[i].name, dl[i].ip, dl[i].slave,
                      modbus_tcp_mfr_name(dl[i].mfr), modbus_tcp_role_name(dl[i].role),
                      dl[i].connected ? 1 : 0, dl[i].pv_w, dl[i].w, dl[i].soc);
        if (o > sizeof(j) - 128) break;
    }
    snprintf(j + o, sizeof(j) - o, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, j);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,
        "multipart/x-mixed-replace;boundary=" MJPEG_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, private");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part[96];
    ESP_LOGI(TAG, "stream client connected");

    while (true) {
        if (s_paused) {                 /* OTA in progress: keep panel static */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint32_t len = 0;
        esp_err_t err = grab_frame(&len);
        if (err != ESP_OK || len == 0) {
            vTaskDelay(pdMS_TO_TICKS(FRAME_PERIOD_MS));
            continue;
        }

        int n = snprintf(part, sizeof(part),
                         "\r\n--" MJPEG_BOUNDARY
                         "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)len);
        if (httpd_resp_send_chunk(req, part, n) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, (const char *)s_out, len) != ESP_OK) break;

        vTaskDelay(pdMS_TO_TICKS(FRAME_PERIOD_MS));
    }

    ESP_LOGI(TAG, "stream client gone");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* --------------------------- public API -------------------------------- */

void web_mirror_pause(bool paused)
{
    s_paused = paused;
}

void web_mirror_register(httpd_handle_t server)
{
    httpd_uri_t page = {
        .uri = "/", .method = HTTP_GET, .handler = page_handler,
    };
    httpd_uri_t touch = {
        .uri = "/touch", .method = HTTP_GET, .handler = touch_handler,
    };
    httpd_uri_t key = {
        .uri = "/key", .method = HTTP_GET, .handler = key_handler,
    };
    httpd_uri_t paste = {
        .uri = "/paste", .method = HTTP_POST, .handler = paste_handler,
    };
    httpd_uri_t copy = {
        .uri = "/copy", .method = HTTP_GET, .handler = copy_handler,
    };
    httpd_uri_t live = {
        .uri = "/api/live", .method = HTTP_GET, .handler = live_handler,
    };
    httpd_uri_t devs = {
        .uri = "/api/devices", .method = HTTP_GET, .handler = devices_handler,
    };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &touch);
    httpd_register_uri_handler(server, &key);
    httpd_register_uri_handler(server, &paste);
    httpd_register_uri_handler(server, &copy);
    httpd_register_uri_handler(server, &live);
    httpd_register_uri_handler(server, &devs);
}

esp_err_t web_mirror_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;

    jpeg_encode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 80 };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_enc), TAG, "jpeg engine");

    jpeg_encode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_in  = jpeg_alloc_encoder_mem(SCR_BYTES, &in_cfg, &s_in_cap);
    s_out = jpeg_alloc_encoder_mem(220 * 1024, &out_cfg, &s_out_cap);
    if (!s_in || !s_out) {
        ESP_LOGE(TAG, "jpeg buffers alloc failed");
        return ESP_ERR_NO_MEM;
    }

    if (app_lvgl_lock(1000)) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, web_indev_read);
        app_lvgl_unlock();
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = STREAM_PORT;
    cfg.ctrl_port        = 32811;          /* distinct from the :80 server */
    /* grab_frame() now just reads the panel framebuffer + rotates + JPEGs (no
     * LVGL draw pipeline), so it needs far less stack than before -- but the
     * JPEG encoder path still wants headroom, so keep it generous. */
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = 2;              /* freed a slot for the :80/OTA server */
    cfg.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_stream_httpd, &cfg), TAG, "stream httpd");

    httpd_uri_t stream = {
        .uri = "/", .method = HTTP_GET, .handler = stream_handler,
    };
    httpd_register_uri_handler(s_stream_httpd, &stream);

    ESP_LOGI(TAG, "web mirror ready: page on :80, MJPEG stream on :%d", STREAM_PORT);
    return ESP_OK;
}

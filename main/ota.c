#include "ota.h"
#include "build_info.h"
#include "assets_fs.h"
#include "lvgl_port.h"
#include "web_mirror.h"
#include "wifi_mgr.h"
#include "display.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_RECV_CHUNK   4096

/* Recovery page embedded from recovery.html (EMBED_TXTFILES). */
extern const char recovery_html_start[] asm("_binary_recovery_html_start");

/* Heavy flash writes during OTA stall the cache; with LVGL still rendering and
 * the web mirror snapshotting, the panel flickers wildly. So freeze the UI for
 * the duration: pause the MJPEG stream and hold the LVGL lock, which stops
 * rendering and leaves the panel showing a static last frame. */
static bool s_frozen;

static void ota_freeze_ui(void)
{
    web_mirror_pause(true);
    s_frozen = app_lvgl_lock(2000);     /* held until ota_thaw_ui() / reboot */
    /* Backlight off is what actually kills the flicker: flash cache stalls
     * disturb the panel scanout regardless of rendering, so blank it. */
    display_backlight(false);
    ESP_LOGW(TAG, "UI frozen + backlight off for flash (lvgl_lock=%d)", s_frozen);
}

static void ota_thaw_ui(void)
{
    display_backlight(true);            /* restore to the saved brightness */
    if (s_frozen) {
        app_lvgl_unlock();
        s_frozen = false;
    }
    web_mirror_pause(false);
    ESP_LOGI(TAG, "UI thawed");
}

/* GET /ota -> what is currently running (handy to confirm an OTA took). */
static esp_err_t ota_info_handler(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *app  = esp_app_get_description();
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);

    char json[360];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"build\":%d,\"fs_build\":%d,\"running\":\"%s\","
             "\"target_slot\":\"%s\",\"idf\":\"%s\",\"mac\":\"%s\",\"uptime\":%lld}",
             DEYE_BUILD_VERSION_FULL, DEYE_BUILD_NUMBER, assets_fs_build_number(),
             run ? run->label : "?",
             next ? next->label : "?",
             app ? app->idf_ver : "?", st.mac,
             (long long)(esp_timer_get_time() / 1000000));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

/* POST /ota -> raw firmware image in the body; flash inactive slot + reboot. */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA start -> slot '%s', %d bytes incoming",
             part->label, req->content_len);

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    ota_freeze_ui();                    /* no display flicker during the flash */

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_RECV_CHUNK);
    if (!buf) {
        esp_ota_abort(handle);
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    int remaining = req->content_len, total = 0;
    while (remaining > 0) {
        int want = remaining < OTA_RECV_CHUNK ? remaining : OTA_RECV_CHUNK;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed (%d) after %d bytes", r, total);
            esp_ota_abort(handle);
            free(buf);
            ota_thaw_ui();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            ota_thaw_ui();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write");
            return ESP_FAIL;
        }
        remaining -= r;
        total += r;
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            err == ESP_ERR_OTA_VALIDATE_FAILED
                                ? "image invalid" : "ota end");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot");
        return ESP_FAIL;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "OTA OK: %d bytes -> %s, rebooting\n",
             total, part->label);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, msg);

    ESP_LOGW(TAG, "OTA complete (%d bytes -> %s), rebooting", total, part->label);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

/* POST /ota/fs -> raw SPIFFS image into the "storage" partition. Applies on
 * the next boot (assets are read at startup), so follow with a reboot or a
 * firmware OTA. Keeps the FS build number in sync with the firmware over WiFi. */
static esp_err_t ota_fs_handler(httpd_req_t *req)
{
    const esp_partition_t *fs = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!fs) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no storage part");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || (uint32_t)req->content_len > fs->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "FS OTA -> '%s' (%d bytes), erasing %u", fs->label,
             req->content_len, (unsigned)fs->size);

    ota_freeze_ui();                    /* no display flicker during the flash */

    esp_err_t err = esp_partition_erase_range(fs, 0, fs->size);
    if (err != ESP_OK) {
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erase");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_RECV_CHUNK);
    if (!buf) {
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    int remaining = req->content_len, written = 0, fill = 0;
    while (remaining > 0) {
        int want = OTA_RECV_CHUNK - fill;
        if (want > remaining) want = remaining;
        int r = httpd_req_recv(req, (char *)buf + fill, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); ota_thaw_ui(); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_FAIL; }
        fill += r; remaining -= r;
        if (fill == OTA_RECV_CHUNK || remaining == 0) {
            while (fill & 3) buf[fill++] = 0xFF;       /* 4-byte align tail */
            err = esp_partition_write(fs, written, buf, fill);
            if (err != ESP_OK) { free(buf); ota_thaw_ui(); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write"); return ESP_FAIL; }
            written += fill; fill = 0;
        }
    }
    free(buf);
    ota_thaw_ui();                       /* FS OTA does not reboot -> resume UI */

    ESP_LOGW(TAG, "FS OTA done (%d bytes) -- applies on next boot", written);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "FS OK, applies on next boot\n");
}

/* POST /ota/reboot -> just restart. */
static esp_err_t ota_reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "rebooting\n");
    ESP_LOGW(TAG, "reboot requested via /ota/reboot");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

/* POST /ota/rollback -> boot the OTHER slot (the previous firmware) + restart.
 * The rescue path when an OTA boots but misbehaves. */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (!other) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no other slot");
        return ESP_FAIL;
    }
    /* validates the image header before switching */
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rollback set_boot '%s': %s", other->label, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "other slot has no valid image");
        return ESP_FAIL;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "switching to %s, rebooting\n", other->label);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, msg);
    ESP_LOGW(TAG, "rollback -> %s, rebooting", other->label);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t recovery_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, recovery_html_start, HTTPD_RESP_USE_STRLEN);
}

void ota_register_routes(httpd_handle_t server)
{
    httpd_uri_t info     = { .uri = "/ota",          .method = HTTP_GET,  .handler = ota_info_handler };
    httpd_uri_t post     = { .uri = "/ota",          .method = HTTP_POST, .handler = ota_post_handler };
    httpd_uri_t fs       = { .uri = "/ota/fs",       .method = HTTP_POST, .handler = ota_fs_handler };
    httpd_uri_t reboot   = { .uri = "/ota/reboot",   .method = HTTP_POST, .handler = ota_reboot_handler };
    httpd_uri_t rollback = { .uri = "/ota/rollback", .method = HTTP_POST, .handler = ota_rollback_handler };
    httpd_uri_t recovery = { .uri = "/recovery",     .method = HTTP_GET,  .handler = recovery_page_handler };
    httpd_register_uri_handler(server, &info);
    httpd_register_uri_handler(server, &post);
    httpd_register_uri_handler(server, &fs);
    httpd_register_uri_handler(server, &reboot);
    httpd_register_uri_handler(server, &rollback);
    httpd_register_uri_handler(server, &recovery);
}

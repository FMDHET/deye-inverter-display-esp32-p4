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
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_RECV_CHUNK   4096

/* The receive buffer is the SOURCE of a flash write, and a flash write runs
 * with the cache disabled -- so the buffer MUST NOT live in PSRAM, whose access
 * path goes through that very cache. With CONFIG_SPIRAM_USE_MALLOC a plain
 * malloc() only PREFERS internal RAM (ALWAYSINTERNAL=16384) and silently falls
 * back to PSRAM when internal RAM is tight, which made this fail at random:
 * the same 1 MB filesystem upload succeeded once and panicked twice.
 * heap_caps_malloc with MALLOC_CAP_INTERNAL takes the guesswork out. */
#define OTA_BUF_CAPS     (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

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

/* httpd_req_recv returns TIMEOUT after recv_wait_timeout (12 s, captive.c) and
 * the old loop simply retried -- forever. A phone that went to sleep at 40 %
 * of an upload therefore left the device with the backlight off, the LVGL
 * lock held and the single httpd task parked here: no /ota/reboot, no
 * /recovery, no touch. Only the power plug helped. Three timeouts (36 s)
 * ride out any WiFi hiccup; after that the upload is abandoned and the UI
 * thawed by the caller's error path. */
#define OTA_RECV_MAX_TIMEOUTS 3
static int ota_recv(httpd_req_t *req, char *buf, int want)
{
    for (int t = 0; t < OTA_RECV_MAX_TIMEOUTS; t++) {
        int r = httpd_req_recv(req, buf, want);
        if (r != HTTPD_SOCK_ERR_TIMEOUT) return r;
        ESP_LOGW(TAG, "recv timeout %d/%d", t + 1, OTA_RECV_MAX_TIMEOUTS);
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

/* Is this a firmware image for THIS device? Checked on the first received
 * bytes, before esp_ota_begin erases the target slot: with OTA_SIZE_UNKNOWN
 * the whole 4 MB rollback slot was wiped before the first byte was looked at,
 * so uploading storage.bin into the firmware field destroyed the only
 * fallback image and left /ota/rollback with "other slot has no valid image".
 * The app descriptor sits right after the image header and the first segment
 * header; its project_name is what tells our firmware from any other P4 app. */
#define OTA_HDR_BYTES (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + \
                       sizeof(esp_app_desc_t))
static const char *ota_image_check(const char *buf, int len)
{
    if (len < (int)OTA_HDR_BYTES) return "image too small";
    const esp_image_header_t *ih = (const esp_image_header_t *)buf;
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC) return "not a firmware image";
    if (ih->chip_id != ESP_CHIP_ID_ESP32P4)  return "firmware for another chip";
    const esp_app_desc_t *ad = (const esp_app_desc_t *)
        (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (ad->magic_word != ESP_APP_DESC_MAGIC_WORD) return "no app descriptor";
    const esp_app_desc_t *me = esp_app_get_description();
    if (me && strncmp(ad->project_name, me->project_name, sizeof(ad->project_name)) != 0)
        return "not a deye_display image";
    return NULL;
}

/* GET /ota -> what is currently running (handy to confirm an OTA took). */
/* Why the chip last came up. A failed OTA that reboots the device leaves no
 * trace anywhere reachable over the network -- there is no serial console on a
 * wall-mounted unit -- so /ota reports it and the next incident is diagnosable
 * instead of guesswork: TASK_WDT and INT_WDT point at a starved or blocking
 * task, PANIC at a crash, BROWNOUT at the power supply, SW at our own reboot. */
static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "SDIO";
    case ESP_RST_USB:      return "USB";
    case ESP_RST_JTAG:     return "JTAG";
    default:               return "UNKNOWN";
    }
}

/* What the bootloader thinks of a slot. VALID/NEW are bootable, PENDING_VERIFY
 * is on probation, INVALID/ABORTED were rejected -- a rollback onto one of
 * those would just reboot into the same image again. */
static const char *ota_state_name(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return "UNKNOWN";
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "?";
    }
}

static esp_err_t ota_info_handler(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *app  = esp_app_get_description();
    esp_app_desc_t other_desc = { 0 };
    bool other_ok = next && esp_ota_get_partition_description(next, &other_desc) == ESP_OK;
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);

    /* heap_min is the low-water mark since boot: an upload that dies from
     * memory pressure in the network stack shows up here even though the value
     * has long recovered by the time anyone asks. */
    /* dma_* is the pool that decides whether an OTA survives: the SDIO driver
     * grows its receive buffer from DMA-capable internal RAM while the flash
     * write is running, and asserts if that allocation fails. The overall heap
     * says nothing about it -- it is PSRAM and always looks roomy. */
    char json[760];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"build\":%d,\"fs_build\":%d,\"running\":\"%s\","
             "\"running_state\":\"%s\","
             "\"target_slot\":\"%s\",\"other_state\":\"%s\",\"other_version\":\"%s\","
             "\"idf\":\"%s\",\"mac\":\"%s\",\"uptime\":%lld,"
             "\"reset\":\"%s\",\"heap\":%u,\"heap_min\":%u,"
             "\"dma\":%u,\"dma_max\":%u,\"dma_min\":%u}",
             DEYE_BUILD_VERSION_FULL, DEYE_BUILD_NUMBER, assets_fs_build_number(),
             run ? run->label : "?", ota_state_name(run),
             next ? next->label : "?", ota_state_name(next),
             other_ok ? other_desc.version : "",
             app ? app->idf_ver : "?", st.mac,
             (long long)(esp_timer_get_time() / 1000000),
             reset_reason_name(),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");   /* the recovery page polls this */
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
    if ((uint32_t)req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }

    char *buf = heap_caps_malloc(OTA_RECV_CHUNK, OTA_BUF_CAPS);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    /* First the header, then the erase -- see ota_image_check(). */
    int have = 0;
    while (have < (int)OTA_HDR_BYTES && have < req->content_len) {
        int r = ota_recv(req, buf + have, OTA_RECV_CHUNK - have);
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed (%d) in header", r);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
            return ESP_FAIL;
        }
        have += r;
    }
    const char *why = ota_image_check(buf, have);
    if (why) {
        ESP_LOGE(TAG, "OTA rejected: %s", why);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
        return ESP_FAIL;
    }

    /* A running image still on probation (see ota_arm_confirm) blocks
     * esp_ota_begin with ESP_ERR_OTA_ROLLBACK_INVALID_STATE. But an OTA
     * request arriving here IS the proof the probation waits for -- the
     * device is reachable and can be reflashed -- so confirm now. Without
     * this, a second update within the first minute after a reboot failed. */
    ota_mark_app_valid();

    ota_freeze_ui();                    /* no display flicker during the flash */

    /* The exact size: only the sectors actually needed get erased, which is
     * also a shorter blackout than wiping the whole 4 MB slot. */
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, req->content_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        free(buf);
        ota_thaw_ui();
        char why[64];
        snprintf(why, sizeof(why), "ota begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, why);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA start: dma_free=%u dma_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    err = esp_ota_write(handle, buf, have);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write (header): %s", esp_err_to_name(err));
        esp_ota_abort(handle);
        free(buf);
        ota_thaw_ui();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write");
        return ESP_FAIL;
    }

    int remaining = req->content_len - have, total = have, last_dma_report = 0;
    while (remaining > 0) {
        int want = remaining < OTA_RECV_CHUNK ? remaining : OTA_RECV_CHUNK;
        int r = ota_recv(req, buf, want);
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

        /* Watch the DMA pool while the write runs. This is where OTAs die: the
         * SDIO driver grows its receive buffer from here, and a failed malloc
         * is an assert inside the driver, not an error we could catch. */
        if (total - last_dma_report >= 512 * 1024) {
            last_dma_report = total;
            ESP_LOGW(TAG, "OTA %d KB: dma_free=%u dma_largest=%u",
                     total / 1024,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        }
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

/* Error exit of the FS handler: whatever is left on the partition gets mounted
 * again (a failed mount is harmless -- fs_build then reads -1), the UI thaws,
 * the client gets its 500. */
static esp_err_t fs_fail(httpd_req_t *req, uint8_t *buf, const char *msg)
{
    ESP_LOGE(TAG, "FS OTA failed: %s", msg);
    free(buf);
    assets_fs_mount();
    ota_thaw_ui();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
}

/* POST /ota/fs -> raw SPIFFS image into the "storage" partition. The FS is
 * unmounted for the write and mounted again afterwards, so the new assets are
 * live immediately -- no reboot needed. Keeps the FS build number in sync with
 * the firmware over WiFi. */
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

    uint8_t *buf = heap_caps_malloc(OTA_RECV_CHUNK, OTA_BUF_CAPS);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    ota_freeze_ui();                    /* no display flicker during the flash */

    /* SPIFFS keeps page and object state in RAM. Writing the partition
     * underneath a mounted FS left that state describing the OLD image:
     * GET /ota reported the stale fs_build until the next boot ("applies on
     * next boot"), and any internal SPIFFS write would have landed on top of
     * the fresh image. Unmount, rewrite, mount again -- fs_build is then
     * correct immediately. */
    assets_fs_unmount();

    /* Erase sector by sector right before each write instead of the whole
     * 1 MB up front: shorter blackout before the first byte arrives, and the
     * image from spiffsgen is always a whole partition, so nothing is left
     * half-erased when the upload completes. */
    int remaining = req->content_len, written = 0, fill = 0;
    while (remaining > 0) {
        int want = OTA_RECV_CHUNK - fill;
        if (want > remaining) want = remaining;
        int r = ota_recv(req, (char *)buf + fill, want);
        if (r <= 0) return fs_fail(req, buf, "recv");
        fill += r; remaining -= r;
        if (fill == OTA_RECV_CHUNK || remaining == 0) {
            while (fill & 3) buf[fill++] = 0xFF;       /* 4-byte align tail */
            esp_err_t err = esp_partition_erase_range(fs, written, OTA_RECV_CHUNK);
            if (err != ESP_OK) return fs_fail(req, buf, "erase");
            err = esp_partition_write(fs, written, buf, fill);
            if (err != ESP_OK) return fs_fail(req, buf, "write");
            written += fill; fill = 0;
        }
    }
    /* A shorter-than-partition image: clear the tail so no stale SPIFFS
     * pages from the previous image survive behind it. */
    if ((uint32_t)written < fs->size) {
        uint32_t from = (written + OTA_RECV_CHUNK - 1) & ~(OTA_RECV_CHUNK - 1);
        if (from < fs->size) esp_partition_erase_range(fs, from, fs->size - from);
    }
    free(buf);
    assets_fs_mount();
    ota_thaw_ui();                       /* FS OTA does not reboot -> resume UI */

    int fs_build = assets_fs_build_number();
    ESP_LOGW(TAG, "FS OTA done (%d bytes) -- fs_build now #%d", written, fs_build);
    char msg[64];
    snprintf(msg, sizeof(msg), "FS OK, build #%d mounted\n", fs_build);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, msg);
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
    /* A slot the bootloader has already rejected (it panicked on probation, or
     * was aborted) would boot as NEW, fail its probation again and flip right
     * back -- the user sees a reboot that changed nothing. Say so instead. */
    esp_ota_img_states_t ost;
    if (esp_ota_get_state_partition(other, &ost) == ESP_OK &&
        (ost == ESP_OTA_IMG_INVALID || ost == ESP_OTA_IMG_ABORTED)) {
        char why[96];
        snprintf(why, sizeof(why), "other slot (%s) is %s -- the bootloader rejected it",
                 other->label, ota_state_name(other));
        ESP_LOGW(TAG, "rollback refused: %s", why);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
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

/* Confirm the running image so the bootloader stops watching it.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly OTA'd image boots in
 * state PENDING_VERIFY and is kept only once the app reports itself healthy.
 * Miss that call -- crash, hang, or simply never getting this far -- and the
 * bootloader reverts to the previous slot on the next boot.
 *
 * That is the safety net for the failure mode this project actually hit: a
 * firmware that asserts before app_main answers on no interface at all, so
 * neither /recovery nor OTA can undo it and only a USB cable helps.
 *
 * "Healthy" is deliberately defined as "the /ota endpoint is up again": if the
 * device can still be reflashed over the network, a bad build is recoverable
 * without physical access, which is the whole point. Demanding more (STA
 * associated, MQTT connected, Deye responding) would make a router outage or an
 * unplugged RS485 line look like a broken firmware and revert a good one. */
void ota_mark_app_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;

    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;   /* nothing to confirm */

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "image on '%s' confirmed -- rollback cancelled", run->label);
    } else {
        ESP_LOGE(TAG, "mark_app_valid failed: %s -- this boot may be reverted",
                 esp_err_to_name(err));
    }
}

/* Probation timer. ota_mark_app_valid() used to be called straight after
 * captive_start() -- before the Modbus, MQTT and WireGuard tasks existed and
 * before WiFi had an IP. A build that panics two seconds into runtime was
 * therefore already confirmed and boot-looped forever, which is exactly what
 * the rollback was built to catch. Now the image is confirmed once the device
 * has been up for a while AND is reachable (STA has an IP, or the SoftAP
 * fallback is up -- the AP comes after 20 s without STA, so a router outage
 * still confirms a good build). Whatever happens, confirm after ten minutes:
 * a UI-only device is still rescuable over USB, and never confirming would
 * revert a good image at the next power cut. */
#define OTA_CONFIRM_MIN_S    60
#define OTA_CONFIRM_MAX_S   600
#define OTA_CONFIRM_TICK_MS 5000
static esp_timer_handle_t s_confirm_timer;

static void confirm_timer_cb(void *arg)
{
    (void)arg;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;
    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        esp_timer_stop(s_confirm_timer);           /* nothing (left) to confirm */
        return;
    }
    int64_t up_s = esp_timer_get_time() / 1000000;
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    bool reachable = (st.state == WIFI_MGR_STA_CONNECTED && st.sta_ip[0]) || st.ap_active;
    if ((reachable && up_s >= OTA_CONFIRM_MIN_S) || up_s >= OTA_CONFIRM_MAX_S) {
        ESP_LOGW(TAG, "probation over after %llds (%s) -- confirming image",
                 (long long)up_s, reachable ? "reachable" : "timeout");
        ota_mark_app_valid();
        esp_timer_stop(s_confirm_timer);
    }
}

void ota_arm_confirm(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;
    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;                                    /* USB-flashed or already confirmed */
    }
    ESP_LOGW(TAG, "image on '%s' is on probation -- confirming after %d s when reachable, "
                  "%d s at the latest", run->label, OTA_CONFIRM_MIN_S, OTA_CONFIRM_MAX_S);
    const esp_timer_create_args_t args = {
        .callback = confirm_timer_cb,
        .name     = "ota_confirm",
    };
    if (esp_timer_create(&args, &s_confirm_timer) == ESP_OK) {
        esp_timer_start_periodic(s_confirm_timer, (uint64_t)OTA_CONFIRM_TICK_MS * 1000);
    } else {
        ota_mark_app_valid();                      /* no timer -> better confirmed than reverted */
    }
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

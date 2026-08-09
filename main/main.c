#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "nvs_store.h"
#include "assets_fs.h"
#include "build_info.h"
#include "fonts.h"
#include "ui_flow.h"
#include "ui_settings.h"
#include "wifi_mgr.h"
#include "captive.h"
#include "web_mirror.h"
#include "ota.h"
#include "modbus_tcp.h"
#include "modbus_rtu.h"
#include "modbus_gw.h"
#include "deye_ctrl.h"
#include "mqtt_fwd.h"
#include "ntp_client.h"
#include "wg_client.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "deye-display";

/* C6 slave firmware upgraded to 2.12.8 (matches the esp_hosted host), so
 * WiFi init no longer reboot-loops. Enabled. */
#define DEYE_ENABLE_WIFI 1

void app_main(void)
{
    ESP_LOGI(TAG, "Deye-Display booting on ESP32-P4");
    ESP_LOGI(TAG, "Firmware build %s  (#%d, %s)",
             DEYE_BUILD_VERSION_FULL, DEYE_BUILD_NUMBER, DEYE_BUILD_TIMESTAMP);

    ESP_ERROR_CHECK(nvs_store_init());

    /* Mount the versioned asset FS and confirm it was flashed in lockstep
     * with this firmware (same build number). Non-fatal if absent. */
    assets_fs_mount();
    int fs_build = assets_fs_build_number();
    if (fs_build == DEYE_BUILD_NUMBER) {
        ESP_LOGI(TAG, "Filesystem build matches firmware (#%d)", fs_build);
    } else if (fs_build < 0) {
        ESP_LOGW(TAG, "Filesystem build unavailable (asset image not flashed?)");
    } else {
        ESP_LOGE(TAG, "BUILD MISMATCH: firmware #%d but filesystem #%d "
                      "-- reflash so FW + FS + UI agree",
                 DEYE_BUILD_NUMBER, fs_build);
    }

    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    ESP_ERROR_CHECK(display_init(&panel, &io));

    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(touch_init(&tp));

    ESP_ERROR_CHECK(app_lvgl_start(panel, io, tp));

    if (app_lvgl_lock(-1)) {
        fonts_init();                 /* tiny-ttf fonts (umlauts) before UI */
        ui_flow_create();
        ui_flow_set_fs_build(fs_build);
        app_lvgl_unlock();
    }
    /* NOTE: ui_settings_create() is deliberately built LATER (after the backend
     * *_start() calls below), so its config tabs render from the loaded NVS
     * config instead of empty caches. */

    /* WiFi via the on-module C6 (ESP-Hosted). Brings up STA from saved
     * credentials, with AP fallback. Once WiFi is up we start the captive
     * portal so a phone joining the SoftAP is auto-redirected to the WLAN
     * setup page. */
#if DEYE_ENABLE_WIFI
    esp_err_t err = wifi_mgr_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_mgr_init failed: %s", esp_err_to_name(err));
    } else {
        /* Live web mirror of the display (MJPEG stream + touch) and the
         * captive portal that auto-opens it when joining the SoftAP. */
        err = web_mirror_init(panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web_mirror_init failed: %s", esp_err_to_name(err));
        }
        err = captive_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "captive_start failed: %s", esp_err_to_name(err));
        } else {
            /* The captive portal's HTTP server is what serves /ota, so this is
             * the exact moment a bad build could still be replaced remotely.
             * Confirm the image here and nowhere earlier -- see ota.c. */
            ota_mark_app_valid();
        }
        /* Poll the Deye inverter over Modbus-TCP and feed the energy-flow UI. */
        err = modbus_tcp_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "modbus_tcp_start failed: %s", esp_err_to_name(err));
        }
        /* Modbus-RTU: emulate an Eastron meter for the Deye + read the Deye. */
        err = modbus_rtu_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "modbus_rtu_start failed: %s", esp_err_to_name(err));
        }
        deye_ctrl_start();   /* async writer task (needs modbus_rtu running) */
        /* Modbus-TCP <-> RTU bridge: serve the RS485 buses (i.e. the Deye's
         * registers) to LAN clients on port 502. Needs modbus_rtu running. */
        err = modbus_gw_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "modbus_gw_start failed: %s", esp_err_to_name(err));
        }
        /* MQTT forwarding (+ HA discovery). */
        err = mqtt_fwd_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mqtt_fwd_start failed: %s", esp_err_to_name(err));
        }
        /* SNTP time sync for the on-screen clock. */
        err = ntp_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ntp_start failed: %s", esp_err_to_name(err));
        }
        /* WireGuard VPN client (brings up the tunnel after NTP syncs). */
        err = wg_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wg_start failed: %s", esp_err_to_name(err));
        }
    }
#else
    ESP_LOGW(TAG, "WiFi disabled");
    /* No network by design, so there is no remote-rescue milestone to wait for.
     * Confirm immediately, otherwise every USB-flashed debug build would be
     * reverted by the bootloader on its second boot. */
    ota_mark_app_valid();
#endif

    /* Build the settings UI now -- AFTER every backend *_start() has run its
     * load_cfg(), so each config tab (Mod TCP/RTU, MQTT, Zeit, VPN, WLAN)
     * renders from the persisted NVS config instead of an empty backend cache.
     * Building it before the loads made saved settings look like they "didn't
     * persist", and saving the empty tab would then overwrite NVS. */
    if (app_lvgl_lock(-1)) {
        ui_settings_create();
        app_lvgl_unlock();
    }

    ESP_LOGI(TAG, "Phase 2 up");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#include "lvgl_port.h"
#include "board_jc4880p443c.h"
#include "nvs_store.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "lvgl_port";

/* The single landscape/portrait display handle, kept so the orientation can be
 * changed at runtime (see app_lvgl_apply_orientation). */
static lv_display_t *s_disp;

void app_lvgl_apply_orientation(uint8_t deg)
{
    /* Only the two LANDSCAPE orientations are supported -- portrait would clip
     * this 800x480 HMI. Normal = LV_DISPLAY_ROTATION_90 (the portrait panel
     * turned landscape); 180 deg (upside-down mounting) = ROTATION_270. Any
     * other/stale value (e.g. a 90 or 270 saved by an earlier firmware) falls
     * back to Normal, so the device can never boot stuck in portrait. */
    lv_display_rotation_t rot = (deg == 180) ? LV_DISPLAY_ROTATION_270
                                             : LV_DISPLAY_ROTATION_90;
    if (s_disp) {
        lv_display_set_rotation(s_disp, rot);
        ESP_LOGI(TAG, "orientation set to %u deg (lv rotation %d)",
                 (unsigned)deg, (int)rot);
    }
}

esp_err_t app_lvgl_start(esp_lcd_panel_handle_t panel,
                         esp_lcd_panel_io_handle_t io,
                         esp_lcd_touch_handle_t tp)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Default task stack (~4 KB) is too tight: the settings-tab refreshers run
     * on the LVGL task and the WiFi/saved-list code uses sizable buffers.
     * Give it real headroom to avoid stack-overflow panics. */
    port_cfg.task_stack = 12 * 1024;
    /* Pin LVGL (render + touch) to core 1. The Modbus poll workers are pinned to
     * core 0 (see modbus_tcp_start), so the UI gets a dedicated core and the
     * touch stays responsive no matter how busy polling/networking is. */
    port_cfg.task_affinity = 1;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "port init");

    /* The display is created at the panel's PHYSICAL portrait resolution
     * (480x800). sw_rotate + lv_display_set_rotation(90) below turns the
     * logical UI into 800x480 landscape, with the PPA doing the rotation
     * in hardware on each flush. */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = (size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres          = BOARD_LCD_H_RES,   /* 480 */
        .vres          = BOARD_LCD_V_RES,   /* 800 */
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = true,   /* rotate in flush (PPA if available) */
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,  /* incompatible with rotation */
        },
    };

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "add_disp_dsi failed");
        return ESP_FAIL;
    }
    s_disp = disp;

    /* Portrait panel -> landscape (normal = 90 deg), plus any saved user
     * orientation (0/90/180/270). */
    lvgl_port_lock(0);
    app_lvgl_apply_orientation(nvs_store_get_orientation());
    lvgl_port_unlock();

    lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = tp,
    };
    if (!lvgl_port_add_touch(&touch_cfg)) {
        ESP_LOGE(TAG, "add_touch failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL up via esp_lvgl_port, %dx%d landscape",
             BOARD_LV_HOR_RES, BOARD_LV_VER_RES);
    return ESP_OK;
}

bool app_lvgl_lock(int timeout_ms)
{
    /* esp_lvgl_port takes 0 to mean "wait forever". */
    uint32_t to = (timeout_ms < 0) ? 0 : (uint32_t)timeout_ms;
    return lvgl_port_lock(to);
}

void app_lvgl_unlock(void)
{
    lvgl_port_unlock();
}

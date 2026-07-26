#include "display.h"
#include "board_jc4880p443c.h"
#include "nvs_store.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"

static const char *TAG = "display";

/* Panel-specific init sequence for the GUITION JC4880P443C 4.3" 480x800
 * ST7701S panel. Source: ESPHome PR #12068 ("JC4880P443 Driver
 * Configuration (ST7701)"). The standard 0x11 / 0x29 sequence is
 * issued by the esp_lcd_st7701 driver after these vendor commands.
 */
static const st7701_lcd_init_cmd_t s_st7701_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09,
                       0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08,
                       0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0,
                       0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0,
                       0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0,
                       0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0,
                       0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    /* Standard MIPI exit-sleep + display-on. The esp_lcd_st7701
     * driver does NOT add these implicitly -- without them the panel
     * stays in sleep mode and the screen never lights up. */
    {0x11, NULL, 0, 120},   /* Sleep Out, wait 120 ms */
    {0x29, NULL, 0, 20},    /* Display On, wait 20 ms */
};

static esp_ldo_channel_handle_t s_ldo_handle;

static esp_err_t ldo_phy_init(void)
{
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = BOARD_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = BOARD_MIPI_DSI_PHY_LDO_MV,
    };
    return esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_handle);
}

/* Backlight is PWM-dimmed via LEDC on BOARD_PIN_LCD_BACKLIGHT. */
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_RES_BITS LEDC_TIMER_10_BIT       /* duty 0..1023 */
/* 25 kHz: above the audible range so the LED boost-converter inductor / MLCCs
 * don't sing. 5 kHz produced a brightness-dependent whine. At 10-bit the LEDC
 * source clock allows up to ~78 kHz, so all 1024 dim steps are kept. */
#define BL_LEDC_FREQ_HZ  25000

static uint8_t s_brightness = 80;                /* percent, 0..100 */

static void backlight_apply(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t max = (1u << 10) - 1;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, (uint32_t)max * pct / 100);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES_BITS,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t ccfg = {
        .gpio_num   = BOARD_PIN_LCD_BACKLIGHT,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,                         /* start dark during bring-up */
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
}

void display_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_brightness = pct;
    backlight_apply(pct);
}

/* on -> last set brightness, off -> 0 (used to hide flash flicker). */
void display_backlight(bool on)
{
    backlight_apply(on ? s_brightness : 0);
}

esp_err_t display_init(esp_lcd_panel_handle_t *panel_out,
                       esp_lcd_panel_io_handle_t *io_out)
{
    ESP_RETURN_ON_ERROR(ldo_phy_init(), TAG, "ldo");
    backlight_init();

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = BOARD_MIPI_DSI_LANE_NUM,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BOARD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io),
                        TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_LCD_PIXEL_CLOCK_HZ / 1000000,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = BOARD_LCD_H_RES,
            .v_size            = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_LCD_HSYNC,
            .hsync_back_porch  = BOARD_LCD_HBP,
            .hsync_front_porch = BOARD_LCD_HFP,
            .vsync_pulse_width = BOARD_LCD_VSYNC,
            .vsync_back_porch  = BOARD_LCD_VBP,
            .vsync_front_porch = BOARD_LCD_VFP,
        },
        .flags.use_dma2d = true,
    };

    st7701_vendor_config_t vendor_cfg = {
        .init_cmds      = s_st7701_init_cmds,
        .init_cmds_size = sizeof(s_st7701_init_cmds) / sizeof(s_st7701_init_cmds[0]),
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
        .flags = {
            .use_mipi_interface = 1,  /* MUST be set or driver picks RGB path */
        },
    };

    esp_lcd_panel_dev_config_t panel_dev_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_dev_cfg, &panel),
                        TAG, "panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel),  TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    /* Apply the saved brightness now that the panel is live. */
    display_set_brightness(nvs_store_get_brightness());

    *panel_out = panel;
    *io_out    = io;

    ESP_LOGI(TAG, "ST7701 %dx%d up, %d lanes @ %d Mbps",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_MIPI_DSI_LANE_NUM, BOARD_MIPI_DSI_LANE_BITRATE_MBPS);
    return ESP_OK;
}

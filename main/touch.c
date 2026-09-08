#include "touch.h"
#include "board_jc4880p443c.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

static void gt911_hw_reset(void)
{
    /* GT911 reset is active-low. The INT pin selects the I2C address
     * during reset (low/float = 0x5D, high = 0x14). Board pulls INT
     * appropriately so we only need to toggle RST.
     */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_TOUCH_RST,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* One probe of the controller at a given I2C address. Frees the panel IO on
 * failure so the caller can try again on the same bus. */
static esp_err_t gt911_probe(i2c_master_bus_handle_t bus, uint8_t addr,
                             const esp_lcd_touch_config_t *tp_cfg,
                             esp_lcd_touch_handle_t *tp_out)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.dev_addr = addr;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io);
    if (err != ESP_OK) return err;

    err = esp_lcd_touch_new_i2c_gt911(tp_io, tp_cfg, tp_out);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(tp_io);
    }
    return err;
}

esp_err_t touch_init(esp_lcd_touch_handle_t *tp_out)
{
    *tp_out = NULL;
    gt911_hw_reset();

    i2c_master_bus_config_t i2c_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = BOARD_I2C_PORT,
        .scl_io_num                   = BOARD_PIN_I2C_SCL,
        .sda_io_num                   = BOARD_PIN_I2C_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &bus), TAG, "i2c bus");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BOARD_LCD_H_RES,
        .y_max        = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = BOARD_PIN_TOUCH_INT,
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    /* The GT911 driver gives up hard if the product-ID read fails. With INT not
     * wired on this board the address cannot be forced, so if 0x5D stays quiet
     * try the alternate 0x14 before declaring the controller absent. Either
     * way the failure is reported, not fatal: main.c runs without touch. */
    esp_err_t err = gt911_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, &tp_cfg, tp_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 at 0x%02X: %s -- trying 0x%02X",
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, esp_err_to_name(err),
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
        err = gt911_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, &tp_cfg, tp_out);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no GT911 found (%s)", esp_err_to_name(err));
        i2c_del_master_bus(bus);
        *tp_out = NULL;
        return err;
    }

    ESP_LOGI(TAG, "GT911 ready");
    return ESP_OK;
}

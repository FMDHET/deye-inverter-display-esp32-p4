#pragma once

/*
 * Hardware definition: GUITION JC4880P443C
 *   ESP32-P4 + ESP32-C6, 4.3" IPS 480x800 MIPI-DSI (ST7701), GT911 touch.
 *
 * Sources for pin map & DSI timing:
 *   - ESPHome PR #12068 (JC4880P443C ST7701)
 *   - agillis/esphome-modular-lvgl-buttons hardware YAML
 *   - Guition JC4880P443C_I_W.zip schematic
 */

/* -------- Panel geometry (native portrait) -------- */
#define BOARD_LCD_H_RES                  480
#define BOARD_LCD_V_RES                  800
#define BOARD_LCD_BITS_PER_PIXEL         16   /* RGB565 framebuffer */
#define BOARD_LCD_COLOR_SPACE_RGB        1

/* -------- MIPI-DSI bus -------- */
#define BOARD_MIPI_DSI_LANE_NUM          2
#define BOARD_MIPI_DSI_LANE_BITRATE_MBPS 500

/* -------- DPI / pixel-clock timing (from ESPHome PR #12068) -------- */
#define BOARD_LCD_PIXEL_CLOCK_HZ         (34 * 1000 * 1000)
#define BOARD_LCD_HSYNC                  12
#define BOARD_LCD_HBP                    42
#define BOARD_LCD_HFP                    42
#define BOARD_LCD_VSYNC                  2
#define BOARD_LCD_VBP                    8
#define BOARD_LCD_VFP                    166

/* -------- LDO channel powering the MIPI DSI PHY -------- */
#define BOARD_MIPI_DSI_PHY_LDO_CHAN      3
#define BOARD_MIPI_DSI_PHY_LDO_MV        2500

/* -------- GPIOs -------- */
#define BOARD_PIN_LCD_RST                5    /* ST7701 reset (active low) */
#define BOARD_PIN_LCD_BACKLIGHT          23   /* PWM via LEDC */

#define BOARD_PIN_I2C_SDA                7
#define BOARD_PIN_I2C_SCL                8
#define BOARD_I2C_PORT                   0
#define BOARD_I2C_HZ                     400000

#define BOARD_PIN_TOUCH_RST              3
#define BOARD_PIN_TOUCH_INT              GPIO_NUM_NC   /* not wired on this board */
#define BOARD_TOUCH_I2C_ADDR             0x5D          /* GT911 default */

/* -------- RS485 / Modbus-RTU (two auto-direction transceivers on the
 * user GPIO header). UART0 is the console, so use UART1 + UART2.
 *   Bus A: ESP is the SLAVE that emulates an Eastron SDM630 on the Deye's
 *          meter port (Deye polls it).
 *   Bus B: ESP is the MASTER that reads the Deye inverter.
 * Auto-direction modules need no DE/RE pin. */
#define BOARD_RS485_A_UART               1
#define BOARD_RS485_A_TX                 52
#define BOARD_RS485_A_RX                 51
#define BOARD_RS485_B_UART               2
#define BOARD_RS485_B_TX                 50
#define BOARD_RS485_B_RX                 49

/* -------- LVGL display orientation --------
 * Panel is mounted at the Deye in landscape with the long edge horizontal.
 * Native is 480 (H) x 800 (V) portrait; we rotate 90°.
 */
#define BOARD_LV_HOR_RES                 800
#define BOARD_LV_VER_RES                 480
#define BOARD_LV_ROTATION_DEG            90

#pragma once
/* Host fake of the UART driver. Reads return "nothing arrived", writes are
 * counted so a test can see THAT something was sent without pretending to
 * simulate a bus. Anything that actually needs the wire is a device test.
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef int uart_port_t;

typedef enum { UART_DATA_8_BITS = 3 } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;
typedef enum { UART_MODE_UART = 0, UART_MODE_RS485_HALF_DUPLEX = 1 } uart_mode_t;

#define UART_PIN_NO_CHANGE (-1)

typedef struct {
    int                     baud_rate;
    uart_word_length_t      data_bits;
    uart_parity_t           parity;
    uart_stop_bits_t        stop_bits;
    uart_hw_flowcontrol_t   flow_ctrl;
    uint8_t                 rx_flow_ctrl_thresh;
    uart_sclk_t             source_clk;
} uart_config_t;

/* What the fake recorded -- for tests that care. */
extern uint8_t  fake_uart_tx[512];
extern size_t   fake_uart_tx_len;
extern int      fake_uart_writes;

esp_err_t uart_driver_install(uart_port_t p, int rx_buf, int tx_buf, int q,
                              void *queue, int flags);
esp_err_t uart_driver_delete(uart_port_t p);
int       uart_is_driver_installed(uart_port_t p);
esp_err_t uart_param_config(uart_port_t p, const uart_config_t *cfg);
esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts);
esp_err_t uart_set_mode(uart_port_t p, uart_mode_t mode);
esp_err_t uart_flush_input(uart_port_t p);
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t ticks);
int       uart_read_bytes(uart_port_t p, void *buf, uint32_t len, TickType_t ticks);
int       uart_write_bytes(uart_port_t p, const void *src, size_t len);

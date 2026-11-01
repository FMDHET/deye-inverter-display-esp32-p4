#pragma once
/* Host fake: modbus_rtu.c registers a shutdown handler to save its long-term
 * sums. There is no shutdown on the host -- a test calls the handler itself. */
#include "esp_err.h"

typedef void (*shutdown_handler_t)(void);
static inline esp_err_t esp_register_shutdown_handler(shutdown_handler_t h) { (void)h; return ESP_OK; }

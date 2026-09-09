#pragma once
/* Host fake of the bits modbus_rtu.c and friends use. Critical sections are
 * no-ops: the tests are single-threaded, which is exactly why they can look at
 * the pure computation without racing the bus tasks. */
#include <stdint.h>
#include <stddef.h>

typedef int      BaseType_t;
typedef uint32_t TickType_t;
typedef int      portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux)      ((void)(mux))
#define portEXIT_CRITICAL(mux)       ((void)(mux))
#define portMAX_DELAY                ((TickType_t)0xFFFFFFFFU)
#define pdTRUE                       1
#define pdFALSE                      0
#define pdPASS                       1
#define pdFAIL                       0
#define pdMS_TO_TICKS(ms)            ((TickType_t)(ms))
#define configTICK_RATE_HZ           1000

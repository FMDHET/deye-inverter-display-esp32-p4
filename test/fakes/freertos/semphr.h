#pragma once
/* Host fake: an uncontended semaphore. Always available, always granted --
 * true for a single-threaded test and nowhere else. */
#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)  { return (SemaphoreHandle_t)1; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s; (void)t; return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }

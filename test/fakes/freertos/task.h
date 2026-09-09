#pragma once
/* Host fake: tasks are never started. A test that needs a bus task would be a
 * hardware test, not a host test -- see test/README.md. */
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;

static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                                     uint32_t stack, void *arg,
                                     unsigned prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = (TaskHandle_t)1;
    return pdPASS;
}
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                                 uint32_t stack, void *arg,
                                                 unsigned prio, TaskHandle_t *out,
                                                 int core)
{
    (void)core;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void vTaskPrioritySet(TaskHandle_t h, unsigned p) { (void)h; (void)p; }
static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear; (void)wait; return 0;
}
static inline BaseType_t xTaskNotifyGive(TaskHandle_t h) { (void)h; return pdTRUE; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }

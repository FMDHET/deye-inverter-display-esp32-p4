#pragma once
/* Host fake: log calls become nothing. Build with -DFAKE_LOG to see them --
 * useful when a test fails and the reason is in a log line. */
#include <stdio.h>

#ifdef FAKE_LOG
#define FAKE_LOG_PRINT(lvl, tag, fmt, ...) \
    fprintf(stderr, "[%s] %s: " fmt "\n", lvl, tag, ##__VA_ARGS__)
#else
#define FAKE_LOG_PRINT(lvl, tag, fmt, ...) ((void)0)
#endif

#define ESP_LOGE(tag, fmt, ...) FAKE_LOG_PRINT("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) FAKE_LOG_PRINT("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) FAKE_LOG_PRINT("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) FAKE_LOG_PRINT("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) FAKE_LOG_PRINT("V", tag, fmt, ##__VA_ARGS__)

#pragma once
/* Host fake: log calls become nothing. Build with -DFAKE_LOG to see them --
 * useful when a test fails and the reason is in a log line. */
#include <stdio.h>

/* `if (0)` instead of `((void)0)`: the arguments still get compiled and
 * type-checked, so a variable that exists only to be logged does not look
 * unused on the host (that produced warnings in production code that is
 * perfectly fine), and the compiler checks the format strings for us. */
#ifdef FAKE_LOG
#define FAKE_LOG_PRINT(lvl, tag, fmt, ...) \
    fprintf(stderr, "[%s] %s: " fmt "\n", lvl, tag, ##__VA_ARGS__)
#else
#define FAKE_LOG_PRINT(lvl, tag, fmt, ...)                                     \
    do {                                                                       \
        if (0) fprintf(stderr, "[%s] %s: " fmt "\n", lvl, tag, ##__VA_ARGS__); \
    } while (0)
#endif

#define ESP_LOGE(tag, fmt, ...) FAKE_LOG_PRINT("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) FAKE_LOG_PRINT("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) FAKE_LOG_PRINT("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) FAKE_LOG_PRINT("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) FAKE_LOG_PRINT("V", tag, fmt, ##__VA_ARGS__)

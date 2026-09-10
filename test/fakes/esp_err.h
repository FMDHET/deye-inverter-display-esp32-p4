#pragma once
/* Host fake of the IDF error type. See test/README.md for the rules these
 * fakes follow: pretend as little as possible, and never pretend to succeed
 * where the real thing can fail. */

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_TIMEOUT         0x107

/* Inline, damit jede Suite ihn hat, ohne fakes.c mitlinken zu muessen --
 * das wuerde in den Suiten, die nvs_store.c einbinden, doppelte Symbole geben. */
static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return "ESP_OK";
    case ESP_FAIL:              return "ESP_FAIL";
    case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:  return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:     return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT:       return "ESP_ERR_TIMEOUT";
    default:                    return "ESP_ERR_?";
    }
}

/* Faithful to the real macro: it aborts. A test that walks into one should
 * die loudly rather than carry on in a state the firmware would never reach. */
#include <stdio.h>
#include <stdlib.h>
#define ESP_ERROR_CHECK(x)                                                     \
    do {                                                                       \
        esp_err_t rc_ = (x);                                                   \
        if (rc_ != ESP_OK) {                                                   \
            fprintf(stderr, "ESP_ERROR_CHECK(%s) = %s at %s:%d\n",             \
                    #x, esp_err_to_name(rc_), __FILE__, __LINE__);             \
            abort();                                                           \
        }                                                                      \
    } while (0)

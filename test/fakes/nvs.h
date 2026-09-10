#pragma once
/* Host fake of the NVS API, faithful in the one way that matters here:
 * nvs_get_blob() does NOT truncate. With a buffer smaller than the stored
 * record it returns ESP_ERR_NVS_INVALID_LENGTH and writes nothing -- that is
 * the real contract (ESP-IDF docs), and it is exactly the behaviour that made
 * a rollback lose the settings. A fake that quietly truncated would make the
 * test pass over a bug. Passing out=NULL asks for the size, as in the real API.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ESP_ERR_NVS_BASE                0x1100
#define ESP_ERR_NVS_NOT_FOUND           (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_LENGTH      (ESP_ERR_NVS_BASE + 0x0a)
#define ESP_ERR_NVS_NO_FREE_PAGES       (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND   (ESP_ERR_NVS_BASE + 0x10)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void      nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *out);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v);

/* Test knobs. */
void fake_nvs_store_reset(void);
void fake_nvs_put(const char *ns, const char *key, const void *data, size_t len);
int  fake_nvs_size(const char *ns, const char *key);   /* -1 = nicht vorhanden */

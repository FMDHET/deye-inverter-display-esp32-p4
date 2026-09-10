/* In-memory NVS. One flat table of (namespace, key) -> bytes; that is all
 * nvs_store.c needs, and keeping it dumb keeps it trustworthy.
 *
 * The one place where being faithful matters is nvs_get_blob(): see nvs.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MAX_ENTRIES 32
#define MAX_NS      8
#define MAX_VAL     1024

typedef struct {
    char    ns[16];
    char    key[16];
    uint8_t val[MAX_VAL];
    size_t  len;
    bool    used;
} entry_t;

static entry_t s_tab[MAX_ENTRIES];
static char    s_ns[MAX_NS][16];
static int     s_ns_n;

int fake_nvs_flash_init_rc = ESP_OK;

void fake_nvs_store_reset(void)
{
    memset(s_tab, 0, sizeof(s_tab));
    memset(s_ns, 0, sizeof(s_ns));
    s_ns_n = 0;
    fake_nvs_flash_init_rc = ESP_OK;
}

static entry_t *find(const char *ns, const char *key, bool create)
{
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (s_tab[i].used && !strcmp(s_tab[i].ns, ns) && !strcmp(s_tab[i].key, key))
            return &s_tab[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (!s_tab[i].used) {
            s_tab[i].used = true;
            snprintf(s_tab[i].ns, sizeof(s_tab[i].ns), "%s", ns);
            snprintf(s_tab[i].key, sizeof(s_tab[i].key), "%s", key);
            s_tab[i].len = 0;
            return &s_tab[i];
        }
    return NULL;
}

void fake_nvs_put(const char *ns, const char *key, const void *data, size_t len)
{
    entry_t *e = find(ns, key, true);
    if (!e || len > MAX_VAL) return;
    memcpy(e->val, data, len);
    e->len = len;
}

int fake_nvs_size(const char *ns, const char *key)
{
    entry_t *e = find(ns, key, false);
    return e ? (int)e->len : -1;
}

/* --------------------------- handles ----------------------------------- */

static const char *ns_of(nvs_handle_t h)
{
    return (h >= 1 && (int)h <= s_ns_n) ? s_ns[h - 1] : "";
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    for (int i = 0; i < s_ns_n; i++)
        if (!strcmp(s_ns[i], ns)) { *out = (nvs_handle_t)(i + 1); return ESP_OK; }
    if (s_ns_n >= MAX_NS) return ESP_FAIL;
    snprintf(s_ns[s_ns_n], sizeof(s_ns[0]), "%s", ns);
    s_ns_n++;
    *out = (nvs_handle_t)s_ns_n;
    return ESP_OK;
}

void      nvs_close(nvs_handle_t h)  { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    entry_t *e = find(ns_of(h), key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    e->used = false;
    return ESP_OK;
}

/* ----------------------------- blobs ----------------------------------- */

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    entry_t *e = find(ns_of(h), key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->len; return ESP_OK; }     /* size query */
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;   /* never truncates */
    memcpy(out, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    entry_t *e = find(ns_of(h), key, true);
    if (!e || len > MAX_VAL) return ESP_FAIL;
    memcpy(e->val, val, len);
    e->len = len;
    return ESP_OK;
}

/* ----------------------------- strings --------------------------------- */

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    entry_t *e = find(ns_of(h), key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val)
{
    return nvs_set_blob(h, key, val, strlen(val) + 1);   /* NUL included, as NVS does */
}

/* ---------------------------- numbers ---------------------------------- */

#define NUM_GET(fn, type)                                                      \
    esp_err_t fn(nvs_handle_t h, const char *key, type *out)                   \
    {                                                                          \
        entry_t *e = find(ns_of(h), key, false);                               \
        if (!e || e->len != sizeof(type)) return ESP_ERR_NVS_NOT_FOUND;        \
        memcpy(out, e->val, sizeof(type));                                     \
        return ESP_OK;                                                         \
    }
#define NUM_SET(fn, type)                                                      \
    esp_err_t fn(nvs_handle_t h, const char *key, type v)                      \
    {                                                                          \
        return nvs_set_blob(h, key, &v, sizeof(v));                            \
    }

NUM_GET(nvs_get_u8,  uint8_t)
NUM_SET(nvs_set_u8,  uint8_t)
NUM_GET(nvs_get_u16, uint16_t)
NUM_SET(nvs_set_u16, uint16_t)
NUM_GET(nvs_get_i32, int32_t)
NUM_SET(nvs_set_i32, int32_t)

/* ---------------------------- partition -------------------------------- */

esp_err_t nvs_flash_init(void)
{
    esp_err_t e = fake_nvs_flash_init_rc;
    fake_nvs_flash_init_rc = ESP_OK;     /* a retry after erase succeeds */
    return e;
}

esp_err_t nvs_flash_erase(void)
{
    fake_nvs_store_reset();
    return ESP_OK;
}

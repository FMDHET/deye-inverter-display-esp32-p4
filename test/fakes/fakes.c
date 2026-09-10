/* The parts of the fakes that need storage or a body.
 *
 * Rule for everything in here: a fake may be simple, but it must not be
 * *dishonest*. Where the real function can fail, the fake must be able to fail
 * too, and a test must be able to make it fail -- otherwise the tests only ever
 * see the happy path, which is the one that was never broken.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "fakes.h"

/* ------------------------------ esp_err -------------------------------- */

const char *esp_err_to_name(esp_err_t err)
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

/* -------------------------------- time --------------------------------- */

int64_t fake_now_us = 0;

/* -------------------------------- UART --------------------------------- */

uint8_t fake_uart_tx[512];
size_t  fake_uart_tx_len;
int     fake_uart_writes;

esp_err_t uart_driver_install(uart_port_t p, int rx_buf, int tx_buf, int q,
                              void *queue, int flags)
{
    (void)p; (void)rx_buf; (void)tx_buf; (void)q; (void)queue; (void)flags;
    return ESP_OK;
}
esp_err_t uart_driver_delete(uart_port_t p) { (void)p; return ESP_OK; }
int       uart_is_driver_installed(uart_port_t p) { (void)p; return 0; }
esp_err_t uart_param_config(uart_port_t p, const uart_config_t *cfg)
{
    (void)p; (void)cfg; return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts)
{
    (void)p; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK;
}
esp_err_t uart_set_mode(uart_port_t p, uart_mode_t mode) { (void)p; (void)mode; return ESP_OK; }
esp_err_t uart_flush_input(uart_port_t p) { (void)p; return ESP_OK; }
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t t) { (void)p; (void)t; return ESP_OK; }

/* A queue a test can fill. Reads hand out at most what was pushed -- a short
 * read is exactly what a half-arrived frame looks like on a real bus, and the
 * code under test has to cope with it. */
static uint8_t s_rx[512];
static size_t  s_rx_len, s_rx_pos;

void fake_uart_rx_push(const void *data, size_t n)
{
    if (s_rx_len + n > sizeof(s_rx)) return;
    memcpy(s_rx + s_rx_len, data, n);
    s_rx_len += n;
}

int uart_read_bytes(uart_port_t p, void *buf, uint32_t len, TickType_t ticks)
{
    (void)p; (void)ticks;
    size_t have = s_rx_len - s_rx_pos;
    size_t n = have < len ? have : len;
    if (n) { memcpy(buf, s_rx + s_rx_pos, n); s_rx_pos += n; }
    return (int)n;
}

int uart_write_bytes(uart_port_t p, const void *src, size_t len)
{
    (void)p;
    fake_uart_writes++;
    if (len <= sizeof(fake_uart_tx)) {
        memcpy(fake_uart_tx, src, len);
        fake_uart_tx_len = len;
    }
    return (int)len;
}

void fake_uart_reset(void)
{
    memset(fake_uart_tx, 0, sizeof(fake_uart_tx));
    fake_uart_tx_len = 0;
    fake_uart_writes = 0;
    s_rx_len = s_rx_pos = 0;
}

/* ------------------------- modbus_tcp (the grid) ------------------------
 * The value the control path reads. A test drives it directly: this is the
 * "is the reading fresh?" gate whose failure caused the 15 kW runaway, so the
 * tests must be able to say "stale" at will. */

bool  fake_grid_fresh;
float fake_grid_w;
bool  fake_grid_phases_fresh;
float fake_grid_phases[3];

bool modbus_tcp_grid_w_fresh(float *out_w, uint32_t max_age_ms)
{
    (void)max_age_ms;
    if (!fake_grid_fresh) return false;
    if (out_w) *out_w = fake_grid_w;
    return true;
}

bool modbus_tcp_grid_phases_fresh(float p[3], uint32_t max_age_ms)
{
    (void)max_age_ms;
    if (!fake_grid_phases_fresh) return false;
    if (p) { p[0] = fake_grid_phases[0]; p[1] = fake_grid_phases[1]; p[2] = fake_grid_phases[2]; }
    return true;
}

float fake_rtu_deye_w, fake_rtu_deye_soc;
bool  fake_rtu_deye_valid;
void modbus_tcp_set_rtu_deye(float w, float soc, bool valid)
{
    fake_rtu_deye_w = w; fake_rtu_deye_soc = soc; fake_rtu_deye_valid = valid;
}

/* --------------------------- nvs_store (flash) -------------------------
 * A dictionary in RAM. `fake_nvs_fail` makes every write fail, so the tests can
 * check what the code does when flash says no -- the real device does say no
 * when a page is full. */

bool fake_nvs_fail;
static int  s_grid_sp;
static uint8_t s_rtu[64];   static size_t s_rtu_len;
static uint8_t s_manip[64]; static size_t s_manip_len;

void fake_nvs_reset(void)
{
    fake_nvs_fail = false;
    s_grid_sp = 0;
    s_rtu_len = 0;
    s_manip_len = 0;
}

int       nvs_store_get_grid_sp(void)        { return s_grid_sp; }
esp_err_t nvs_store_set_grid_sp(int w)
{
    if (fake_nvs_fail) return ESP_FAIL;
    s_grid_sp = w; return ESP_OK;
}

esp_err_t nvs_store_get_mb_rtu(void *buf, size_t len)
{
    if (s_rtu_len == 0) return ESP_ERR_NOT_FOUND;
    memcpy(buf, s_rtu, len < s_rtu_len ? len : s_rtu_len);
    return ESP_OK;
}
esp_err_t nvs_store_set_mb_rtu(const void *buf, size_t len)
{
    if (fake_nvs_fail) return ESP_FAIL;
    if (len > sizeof(s_rtu)) return ESP_ERR_INVALID_SIZE;
    memcpy(s_rtu, buf, len); s_rtu_len = len; return ESP_OK;
}

esp_err_t nvs_store_get_mb_manip(void *buf, size_t len)
{
    if (s_manip_len == 0) return ESP_ERR_NOT_FOUND;
    memcpy(buf, s_manip, len < s_manip_len ? len : s_manip_len);
    return ESP_OK;
}
esp_err_t nvs_store_set_mb_manip(const void *buf, size_t len)
{
    if (fake_nvs_fail) return ESP_FAIL;
    if (len > sizeof(s_manip)) return ESP_ERR_INVALID_SIZE;
    memcpy(s_manip, buf, len); s_manip_len = len; return ESP_OK;
}

/* ------------------------- nvs_store: web password ---------------------
 * webauth.c caches this, so a test that changes it must go through
 * web_auth_set() -- fake_web_pw_set() is only for setting the STARTING point
 * before the first read, i.e. "this device already had a password". */
static char s_web_pw[64];

void fake_web_pw_set(const char *pw)
{
    snprintf(s_web_pw, sizeof(s_web_pw), "%s", pw ? pw : "");
}

esp_err_t nvs_store_get_web_pw(char *pw, size_t pw_sz)
{
    if (!pw || pw_sz == 0) return ESP_ERR_INVALID_ARG;
    snprintf(pw, pw_sz, "%s", s_web_pw);
    return ESP_OK;
}

esp_err_t nvs_store_set_web_pw(const char *pw)
{
    if (fake_nvs_fail) return ESP_FAIL;
    fake_web_pw_set(pw);
    return ESP_OK;
}

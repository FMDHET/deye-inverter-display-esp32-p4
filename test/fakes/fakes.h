#pragma once
/* Knobs the tests turn. Everything here is fake state, not production state. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- the grid reading the control path sees (modbus_tcp_grid_w_fresh) --- */
extern bool  fake_grid_fresh;         /* false = "stale", the safety case */
extern float fake_grid_w;
extern bool  fake_grid_phases_fresh;
extern float fake_grid_phases[3];

/* --- what modbus_rtu handed back to modbus_tcp --- */
extern float fake_rtu_deye_w, fake_rtu_deye_soc;
extern bool  fake_rtu_deye_valid;

/* --- UART --- */
extern uint8_t fake_uart_tx[512];
extern size_t  fake_uart_tx_len;
extern int     fake_uart_writes;
void fake_uart_reset(void);

/* --- NVS: set fake_nvs_fail to make every write fail --- */
extern bool fake_nvs_fail;
void fake_nvs_reset(void);

/* --- HTTP: the request header a test hands in, and what came back --- */
extern char fake_req_auth_hdr[256];
extern char fake_resp_status[64];
extern char fake_resp_www_auth[128];
extern char fake_resp_body[512];
extern int  fake_resp_sends;
void fake_http_reset(void);

/* --- the stored web password (nvs_store_get/set_web_pw) --- */
void fake_web_pw_set(const char *pw);

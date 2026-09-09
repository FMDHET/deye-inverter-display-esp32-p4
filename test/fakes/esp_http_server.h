#pragma once
/* Host fake of the bits of esp_http_server that the request handlers touch.
 * A request is a struct the test fills in; a response is recorded so the test
 * can check the status and the headers instead of a socket. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t -- glibc does not get it via stdio.h */
#include "esp_err.h"

typedef struct httpd_req {
    const char *uri;
    size_t      content_len;
    void       *user_ctx;
} httpd_req_t;

typedef void *httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;

typedef struct {
    const char    *uri;
    httpd_method_t method;
    esp_err_t    (*handler)(httpd_req_t *r);
    void          *user_ctx;
} httpd_uri_t;

#define HTTPD_400_BAD_REQUEST            400
#define HTTPD_500_INTERNAL_SERVER_ERROR  500
#define HTTPD_404_NOT_FOUND              404

/* --- what the fake recorded (see fakes.h for the reset helper) --- */
extern char fake_req_auth_hdr[256];   /* "" = header absent */
extern char fake_resp_status[64];
extern char fake_resp_www_auth[128];
extern char fake_resp_body[512];
extern int  fake_resp_sends;

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field,
                                      char *val, size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *qry, const char *key,
                                char *val, size_t val_size);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u);
int       httpd_req_recv(httpd_req_t *r, char *buf, size_t len);

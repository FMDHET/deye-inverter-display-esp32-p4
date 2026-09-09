#include "webauth.h"
#include "nvs_store.h"

#include <string.h>
#include <strings.h>   /* strncasecmp */

#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "webauth";

/* Cached so the gate does not hit NVS on every request (the /deye page polls
 * several endpoints per second). Kept in step by web_auth_set(). */
static char s_pw[WEB_AUTH_PW_MAX];
static bool s_loaded;

static void load(void)
{
    if (s_loaded) return;
    if (nvs_store_get_web_pw(s_pw, sizeof(s_pw)) != ESP_OK) s_pw[0] = '\0';
    s_loaded = true;
}

bool web_auth_enabled(void)
{
    load();
    return s_pw[0] != '\0';
}

esp_err_t web_auth_get(char *out, size_t cap)
{
    if (!out || cap == 0) return ESP_ERR_INVALID_ARG;
    load();
    strncpy(out, s_pw, cap - 1);
    out[cap - 1] = '\0';
    return ESP_OK;
}

esp_err_t web_auth_set(const char *pw)
{
    if (!pw) return ESP_ERR_INVALID_ARG;
    esp_err_t e = nvs_store_set_web_pw(pw);
    if (e == ESP_OK) {
        strncpy(s_pw, pw, sizeof(s_pw) - 1);
        s_pw[sizeof(s_pw) - 1] = '\0';
        s_loaded = true;
        ESP_LOGW(TAG, "web password %s", s_pw[0] ? "set -- writing endpoints now need it"
                                                 : "cleared -- writing endpoints are open again");
    }
    return e;
}

/* Compares in constant time for equal lengths, so the answer time says nothing
 * about HOW MUCH of the password was right. The length itself is not hidden --
 * pretending otherwise would only make this function harder to read. */
static bool pw_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

static bool deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Deye-Display\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Passwort erforderlich. Gesetzt wird es am Display "
                            "unter Einstellungen -> System.\n");
    return false;
}

bool web_auth_ok(httpd_req_t *req)
{
    load();
    if (s_pw[0] == '\0') return true;              /* gate off */

    /* "Authorization: Basic <base64(user:pass)>" -- the user part is ignored,
     * there is exactly one secret on this device and a browser insists on
     * showing a user field. */
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return deny(req);
    if (strncasecmp(hdr, "Basic ", 6) != 0) return deny(req);

    const char *b64 = hdr + 6;
    while (*b64 == ' ') b64++;
    unsigned char dec[96];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                              (const unsigned char *)b64, strlen(b64)) != 0)
        return deny(req);
    dec[dlen] = '\0';

    char *colon = strchr((char *)dec, ':');
    if (!colon) return deny(req);
    if (!pw_equal(colon + 1, s_pw)) {
        ESP_LOGW(TAG, "wrong password on %s", req->uri);
        return deny(req);
    }
    return true;
}

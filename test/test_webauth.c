#include "harness.h"
#include "fakes.h"

#include "esp_http_server.h"
#include "mbedtls/base64.h"

/* Included as source for the same reason as modbus_rtu.c: pw_equal() is static.
 * Here it matters more than usual -- this file decides who may write to the
 * inverter, so "it compiles" is not the interesting property. */
#include "webauth.c"

/* Builds the header a browser would send. */
static void set_basic(const char *user, const char *pass)
{
    char raw[128], b64[256];
    snprintf(raw, sizeof(raw), "%s:%s", user, pass);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen,
                          (const unsigned char *)raw, strlen(raw));
    snprintf(fake_req_auth_hdr, sizeof(fake_req_auth_hdr), "Basic %s", b64);
}

static void start_with(const char *stored_pw)
{
    fake_nvs_reset();
    fake_http_reset();
    fake_web_pw_set(stored_pw);
    s_loaded = false;                 /* forget the cache, re-read from "NVS" */
    s_pw[0]  = '\0';
}

/* -------- the default: no password means nothing changes for anyone ------ */

static void test_no_password_lets_everything_through(void)
{
    start_with("");
    httpd_req_t req = { .uri = "/ota" };

    CHECK(!web_auth_enabled());
    CHECK(web_auth_ok(&req));
    CHECK_I(fake_resp_sends, 0);      /* and nothing was answered on its own */
}

/* ------------------------- the gate, when armed ------------------------- */

static void test_missing_header_is_refused_with_a_prompt(void)
{
    start_with("geheim");
    httpd_req_t req = { .uri = "/ota" };

    CHECK(web_auth_enabled());
    CHECK(!web_auth_ok(&req));
    CHECK(strstr(fake_resp_status, "401") != NULL);
    /* Without this header a browser shows a blank error instead of asking. */
    CHECK(strstr(fake_resp_www_auth, "Basic") != NULL);
    CHECK_I(fake_resp_sends, 1);
}

static void test_correct_password_passes(void)
{
    start_with("geheim");
    httpd_req_t req = { .uri = "/deye/write" };

    set_basic("admin", "geheim");
    CHECK(web_auth_ok(&req));
    CHECK_I(fake_resp_sends, 0);

    /* The user name is not a credential here -- there is one secret on the
     * device, and a browser insists on offering a user field. */
    set_basic("", "geheim");
    CHECK(web_auth_ok(&req));
    set_basic("wer-auch-immer", "geheim");
    CHECK(web_auth_ok(&req));
}

static void test_wrong_password_is_refused(void)
{
    start_with("geheim");
    httpd_req_t req = { .uri = "/ota" };

    set_basic("admin", "falsch");
    CHECK(!web_auth_ok(&req));

    /* A prefix must not pass -- that would be a length-only comparison. */
    fake_http_reset(); fake_web_pw_set("geheim"); s_loaded = false; s_pw[0] = '\0';
    set_basic("admin", "gehe");
    CHECK(!web_auth_ok(&req));

    /* Neither must a longer string that starts with the password. */
    fake_http_reset(); fake_web_pw_set("geheim"); s_loaded = false; s_pw[0] = '\0';
    set_basic("admin", "geheimX");
    CHECK(!web_auth_ok(&req));

    /* Empty password sent while one is required. */
    fake_http_reset(); fake_web_pw_set("geheim"); s_loaded = false; s_pw[0] = '\0';
    set_basic("admin", "");
    CHECK(!web_auth_ok(&req));
}

static void test_malformed_headers_are_refused(void)
{
    httpd_req_t req = { .uri = "/ota" };

    const char *bad[] = {
        "Bearer abcdef",                     /* wrong scheme          */
        "Basic",                             /* no payload            */
        "Basic !!!!not-base64!!!!",          /* undecodable           */
        "Basic YWRtaW4=",                    /* "admin" -- no colon   */
        "geheim",                            /* the raw password      */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        start_with("geheim");
        snprintf(fake_req_auth_hdr, sizeof(fake_req_auth_hdr), "%s", bad[i]);
        CHECK(!web_auth_ok(&req));
    }

    /* Lower-case scheme, though, is legal HTTP and must work. */
    start_with("geheim");
    set_basic("admin", "geheim");
    memcpy(fake_req_auth_hdr, "basic", 5);
    CHECK(web_auth_ok(&req));
}

static void test_password_may_contain_a_colon(void)
{
    /* Only the FIRST colon separates user from password, so a colon inside the
     * password has to survive. */
    start_with("pa:ss:wort");
    httpd_req_t req = { .uri = "/ota" };
    set_basic("admin", "pa:ss:wort");
    CHECK(web_auth_ok(&req));

    start_with("pa:ss:wort");
    set_basic("admin", "pa:ss");
    CHECK(!web_auth_ok(&req));
}

/* ---------------------- setting and clearing it ------------------------- */

static void test_set_and_clear(void)
{
    start_with("");
    httpd_req_t req = { .uri = "/ota" };

    CHECK_I(web_auth_set("neu"), ESP_OK);
    CHECK(web_auth_enabled());
    set_basic("admin", "neu");
    CHECK(web_auth_ok(&req));

    /* Clearing must actually open the door again, not just look empty. */
    CHECK_I(web_auth_set(""), ESP_OK);
    CHECK(!web_auth_enabled());
    fake_http_reset();
    CHECK(web_auth_ok(&req));

    /* And what comes back out is what went in. */
    char buf[WEB_AUTH_PW_MAX];
    web_auth_set("abc123");
    web_auth_get(buf, sizeof(buf));
    CHECK(strcmp(buf, "abc123") == 0);
}

static void test_a_failed_write_does_not_arm_the_gate(void)
{
    /* If flash refuses, the device must not believe it has a password the
     * owner cannot reproduce after a reboot. */
    start_with("");
    fake_nvs_fail = true;
    CHECK(web_auth_set("neu") != ESP_OK);
    CHECK(!web_auth_enabled());

    httpd_req_t req = { .uri = "/ota" };
    CHECK(web_auth_ok(&req));
}

static void test_max_length_password(void)
{
    char longpw[WEB_AUTH_PW_MAX];
    memset(longpw, 'x', sizeof(longpw) - 1);
    longpw[sizeof(longpw) - 1] = '\0';

    start_with(longpw);
    httpd_req_t req = { .uri = "/ota" };
    set_basic("admin", longpw);
    CHECK(web_auth_ok(&req));
}

int main(void)
{
    RUN(test_no_password_lets_everything_through);
    RUN(test_missing_header_is_refused_with_a_prompt);
    RUN(test_correct_password_passes);
    RUN(test_wrong_password_is_refused);
    RUN(test_malformed_headers_are_refused);
    RUN(test_password_may_contain_a_colon);
    RUN(test_set_and_clear);
    RUN(test_a_failed_write_does_not_arm_the_gate);
    RUN(test_max_length_password);
    return t_report("webauth");
}

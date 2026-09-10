#include "harness.h"
#include "nvs.h"
#include "nvs_flash.h"

/* nvs_store.c owns every setting the device has. The interesting part is
 * get_blob_prefix(), and its most important case -- a record written by a
 * NEWER firmware -- cannot be produced on the device at all: it would take a
 * future version to write it. So it gets produced here. */
#include "nvs_store.c"

/* Stand-ins for two struct layouts of the same config: v1 is what an older
 * firmware knows, v2 what a newer one writes after appending a field. */
typedef struct { uint8_t a; uint16_t b; char s[8]; } cfg_v1_t;
typedef struct { uint8_t a; uint16_t b; char s[8]; uint32_t added; } cfg_v2_t;

static void reset(void) { fake_nvs_store_reset(); }

/* ------------------------- the normal case ----------------------------- */

static void test_round_trip(void)
{
    reset();
    cfg_v1_t in = { .a = 7, .b = 4711, .s = "hallo" };
    CHECK_I(nvs_store_set_mqtt(&in, sizeof(in)), ESP_OK);

    cfg_v1_t out;
    memset(&out, 0, sizeof(out));
    CHECK_I(nvs_store_get_mqtt(&out, sizeof(out)), ESP_OK);
    CHECK_I(out.a, 7);
    CHECK_I(out.b, 4711);
    CHECK(strcmp(out.s, "hallo") == 0);
}

static void test_missing_key_is_reported_not_invented(void)
{
    reset();
    cfg_v1_t out;
    memset(&out, 0xAA, sizeof(out));
    CHECK(nvs_store_get_mqtt(&out, sizeof(out)) != ESP_OK);
    /* And it must not have written anything into the caller's buffer -- the
     * modules rely on "error = my pre-zeroed defaults stand". */
    CHECK_I(out.a, 0xAA);
}

/* ---------------- older record: the fields that did not exist ---------- */

static void test_a_shorter_record_keeps_its_fields(void)
{
    reset();
    cfg_v1_t old = { .a = 3, .b = 1234, .s = "alt" };
    fake_nvs_put("mqtt", "cfg", &old, sizeof(old));

    cfg_v2_t out;
    memset(&out, 0, sizeof(out));           /* the caller pre-zeroes */
    CHECK_I(nvs_store_get_mqtt(&out, sizeof(out)), ESP_OK);

    CHECK_I(out.a, 3);
    CHECK_I(out.b, 1234);
    CHECK(strcmp(out.s, "alt") == 0);
    CHECK_I(out.added, 0);                  /* stays 0 -> module default */
}

/* ------------- newer record: the rollback that used to lose it --------- */

static void test_a_longer_record_is_read_not_rejected(void)
{
    reset();
    cfg_v2_t newer = { .a = 5, .b = 2345, .s = "neu", .added = 0xDEADBEEF };
    fake_nvs_put("mqtt", "cfg", &newer, sizeof(newer));

    /* An older firmware reads with its own, smaller struct. Before the fix
     * nvs_get_blob answered ESP_ERR_NVS_INVALID_LENGTH, the module saw "no
     * config" and fell back to defaults -- and overwrote the record with them
     * at the next save. Everything the owner had typed, gone. */
    cfg_v1_t out;
    memset(&out, 0, sizeof(out));
    CHECK_I(nvs_store_get_mqtt(&out, sizeof(out)), ESP_OK);

    CHECK_I(out.a, 5);
    CHECK_I(out.b, 2345);
    CHECK(strcmp(out.s, "neu") == 0);

    /* Reading must not have shortened the stored record: the newer firmware
     * has to find its extra field again after a roll-forward. */
    CHECK_I(fake_nvs_size("mqtt", "cfg"), (int)sizeof(cfg_v2_t));
}

/* The same rule has to hold for every config that uses it, or the next
 * rollback loses whichever one was forgotten. */
static void test_every_blob_reader_follows_the_prefix_rule(void)
{
    struct {
        const char *ns, *key;
        esp_err_t (*get)(void *buf, size_t len);
    } readers[] = {
        { "mqtt",   "cfg",    nvs_store_get_mqtt   },
        { "ntp",    "cfg",    nvs_store_get_ntp    },
        { "wg",     "cfg",    nvs_store_get_wg     },
        { "modbus", "rtu2",   nvs_store_get_mb_rtu },
        { "modbus", "manip1", nvs_store_get_mb_manip },
    };

    for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); i++) {
        reset();
        cfg_v2_t newer = { .a = (uint8_t)(i + 1), .b = 999, .s = "x", .added = 1 };
        fake_nvs_put(readers[i].ns, readers[i].key, &newer, sizeof(newer));

        cfg_v1_t out;
        memset(&out, 0, sizeof(out));
        CHECK_I(readers[i].get(&out, sizeof(out)), ESP_OK);
        CHECK_I(out.a, (int)(i + 1));
        CHECK_I(out.b, 999);
    }
}

/* ------------------------- the plain values ---------------------------- */

static void test_scalars_round_trip_and_default(void)
{
    reset();
    /* Nothing stored -> the documented defaults, not zero. */
    CHECK_I(nvs_store_get_brightness(), 80);
    CHECK_I(nvs_store_get_sls_a(), 35);
    CHECK_I(nvs_store_get_mb_port(), 502);
    CHECK_I(nvs_store_get_grid_sp(), 0);
    CHECK_I(nvs_store_get_deye_mode(), 0);

    nvs_store_set_brightness(25);
    nvs_store_set_sls_a(63);
    nvs_store_set_grid_sp(-350);
    nvs_store_set_deye_power(4000);
    CHECK_I(nvs_store_get_brightness(), 25);
    CHECK_I(nvs_store_get_sls_a(), 63);
    CHECK_I(nvs_store_get_grid_sp(), -350);
    CHECK_I(nvs_store_get_deye_power(), 4000);

    /* A negative setpoint is the normal case here (export), so it must
     * survive the trip through NVS as a signed value. */
    nvs_store_set_grid_sp(-30000);
    CHECK_I(nvs_store_get_grid_sp(), -30000);
}

static void test_ap_password_falls_back_to_the_documented_default(void)
{
    reset();
    char psk[64] = "";
    CHECK_I(nvs_store_get_ap_psk(psk, sizeof(psk)), ESP_OK);
    CHECK(strcmp(psk, "deyedisplay") == 0);   /* the published constant */

    nvs_store_set_ap_psk("etwas-eigenes");
    memset(psk, 0, sizeof(psk));
    nvs_store_get_ap_psk(psk, sizeof(psk));
    CHECK(strcmp(psk, "etwas-eigenes") == 0);
}

static void test_web_password_is_empty_until_set(void)
{
    reset();
    char pw[32] = "x";
    CHECK_I(nvs_store_get_web_pw(pw, sizeof(pw)), ESP_OK);
    CHECK_I(pw[0], '\0');                     /* empty = gate off */

    nvs_store_set_web_pw("geheim");
    nvs_store_get_web_pw(pw, sizeof(pw));
    CHECK(strcmp(pw, "geheim") == 0);
}

/* -------------------------- a damaged partition ------------------------ */

static void test_init_erases_a_damaged_partition(void)
{
    reset();
    nvs_store_set_sls_a(63);
    fake_nvs_flash_init_rc = ESP_ERR_NVS_NO_FREE_PAGES;

    CHECK_I(nvs_store_init(), ESP_OK);        /* erases and retries */
    CHECK_I(nvs_store_get_sls_a(), 35);       /* settings are gone, as expected */
}

int main(void)
{
    RUN(test_round_trip);
    RUN(test_missing_key_is_reported_not_invented);
    RUN(test_a_shorter_record_keeps_its_fields);
    RUN(test_a_longer_record_is_read_not_rejected);
    RUN(test_every_blob_reader_follows_the_prefix_rule);
    RUN(test_scalars_round_trip_and_default);
    RUN(test_ap_password_falls_back_to_the_documented_default);
    RUN(test_web_password_is_empty_until_set);
    RUN(test_init_erases_a_damaged_partition);
    return t_report("nvs_store");
}

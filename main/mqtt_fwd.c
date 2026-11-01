#include "mqtt_fwd.h"
#include "modbus_tcp.h"
#include "nvs_store.h"
#include "deye_ctrl.h"
#include "em_cfg.h"
#include "energy_mgr.h"
#include "price_feed.h"
#include "fronius_storage.h"
#include "bat_lock.h"
#include "deye_force.h"
#include "modbus_rtu.h"
#include "byd_energy.h"
#include "deye_ctrl.h"

#include <stdlib.h>
#include <math.h>
#include <time.h>

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt";

static mqtt_cfg_t              s_cfg;
static mqtt_fwd_status_t       s_st;
static esp_mqtt_client_handle_t s_client;
static portMUX_TYPE            s_mux = portMUX_INITIALIZER_UNLOCKED;

static char s_devid[20];                 /* "deyedisp_aabbcc"        */
static char s_t_avail[64], s_t_state[80];/* availability / state topics */
static char s_t_mode_cmd[96], s_t_pwr_cmd[96]; /* Deye control command topics */
static char s_t_em[80], s_t_em_prices[96], s_t_em_cmd[96];  /* energy manager */
static bool s_loaded;                    /* config loaded from NVS yet? */

/* HA select labels for the three Deye modes (cmd_t payloads + state value). */
static const char *deye_ha_mode(deye_mode_t m)
{
    switch (m) {
    case DEYE_MODE_FORCE_CHARGE:    return "Laden";
    case DEYE_MODE_FORCE_DISCHARGE: return "Entladen";
    case DEYE_MODE_NORMAL:
    default:                        return "Normal";
    }
}

/* DEYE_MODE_COUNT = not one of ours. Anything else on the command topic used
 * to map to NORMAL and be APPLIED -- a typo in an automation reset the
 * inverter's battery mode. Unknown payloads are rejected instead. */
static deye_mode_t deye_mode_from_ha(const char *s)
{
    if (!strcmp(s, "Laden"))    return DEYE_MODE_FORCE_CHARGE;
    if (!strcmp(s, "Entladen")) return DEYE_MODE_FORCE_DISCHARGE;
    if (!strcmp(s, "Normal"))   return DEYE_MODE_NORMAL;
    return DEYE_MODE_COUNT;
}

/* HA-discovery sensor table (value_json keys match the state JSON below). */
static const struct { const char *key, *name, *unit, *dc; } SENS[] = {
    { "pv_w",     "PV Leistung",   "W", "power"   },
    { "house_w",  "Hausverbrauch", "W", "power"   },
    { "grid_w",   "Netz",          "W", "power"   },
    { "byd_w",    "BYD Leistung",  "W", "power"   },
    { "byd_soc",  "BYD SoC",       "%", "battery" },
    { "deye_w",   "Deye Leistung", "W", "power"   },
    { "deye_soc", "Deye SoC",      "%", "battery" },
};
#define N_SENS (sizeof(SENS) / sizeof(SENS[0]))

/* Energy counters of the Eltako meters, one topic per meter role:
 * <base>/meter/<key> = {"import_kwh":..,"export_kwh":..}. Both counters come
 * from one Modbus read, so they always travel together -- a key missing from
 * a shared JSON would make HA log template errors. The first enabled Eltako
 * device of a role is the one reported. */
static const struct { uint8_t role; const char *key, *imp, *exp; } EN_ROLE[] = {
    { MB_ROLE_GRID,       "grid",     "Netz Bezug",        "Netz Einspeisung"     },
    { MB_ROLE_PRODMETER,  "pv_meter", "PV-Zähler Bezug",   "PV-Zähler Lieferung"  },
    { MB_ROLE_DEYE_METER, "deye_ac",  "Deye-Zähler Bezug", "Deye-Zähler Lieferung" },
};
#define N_EN_ROLE (sizeof(EN_ROLE) / sizeof(EN_ROLE[0]))
/* A reading older than this is not sent again (meter gone quiet). */
#define EN_MAX_AGE_MS (6 * MB_ENERGY_POLL_MS)

/* Fill in what the user left unset. Applied to the RAM copy only -- NVS keeps
 * an empty field as 0/"" ("not configured"), so a device that never chose a
 * value still picks up a changed default later instead of having the UI's
 * literal frozen into its config. */
static void normalize_cfg(mqtt_cfg_t *c)
{
    if (c->port == 0)       c->port = 1883;
    if (c->base[0] == '\0') snprintf(c->base, sizeof(c->base), "deye-display");
}

static void load_cfg(void)
{
    mqtt_cfg_t c;
    memset(&c, 0, sizeof(c));
    if (nvs_store_get_mqtt(&c, sizeof(c)) != ESP_OK) {
        c.retain = 1; c.discovery = 1; c.lastwill = 1;
    }
    normalize_cfg(&c);
    portENTER_CRITICAL(&s_mux);
    s_cfg = c;
    s_loaded = true;
    portEXIT_CRITICAL(&s_mux);
}

static void publish_discovery(void)
{
    if (!s_cfg.discovery || !s_client) return;
    for (unsigned i = 0; i < N_SENS; i++) {
        char topic[120], payload[420];
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/%s/%s/config", s_devid, SENS[i].key);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
            "\"val_tpl\":\"{{ value_json.%s }}\",\"unit_of_meas\":\"%s\","
            "\"dev_cla\":\"%s\",\"stat_cla\":\"measurement\",\"uniq_id\":\"%s_%s\","
            "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
            SENS[i].name, s_t_state, s_t_avail, SENS[i].key, SENS[i].unit,
            SENS[i].dc, s_devid, SENS[i].key, s_devid);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }

    /* Control entities: a select for the Deye mode + a number slider for power.
     * Both report their current value from the shared state JSON.
     *
     * When control is switched off they are DELETED, not merely skipped: a
     * discovery config is a retained message, so leaving it in place would keep
     * two controls in Home Assistant that quietly do nothing -- worse than no
     * controls at all. An empty retained payload on the config topic is how HA
     * is told to forget an entity. The sensors above stay either way, so
     * reporting keeps working. */
    {
        /* payload: the "Deye Leistung" entity needs up to 567 bytes with the
         * longest topics the buffers above allow; 560 would cut the JSON short
         * and HA would ignore the entity. */
        char sel_topic[120], num_topic[120], payload[640];
        snprintf(sel_topic, sizeof(sel_topic),
                 "homeassistant/select/%s/deye_mode/config", s_devid);
        snprintf(num_topic, sizeof(num_topic),
                 "homeassistant/number/%s/deye_power/config", s_devid);

        if (s_cfg.deny_ctrl) {
            /* Empty retained payload = "forget this entity". */
            esp_mqtt_client_publish(s_client, sel_topic, "", 0, 1, true);
            esp_mqtt_client_publish(s_client, num_topic, "", 0, 1, true);
            ESP_LOGW(TAG, "MQTT control is off -- the two HA controls were removed");
        } else {
            snprintf(payload, sizeof(payload),
                "{\"name\":\"Deye Modus\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                "\"avty_t\":\"%s\",\"val_tpl\":\"{{ value_json.deye_mode }}\","
                "\"options\":[\"Normal\",\"Laden\",\"Entladen\"],"
                "\"uniq_id\":\"%s_deye_mode\",\"icon\":\"mdi:home-battery\","
                "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
                s_t_mode_cmd, s_t_state, s_t_avail, s_devid, s_devid);
            esp_mqtt_client_publish(s_client, sel_topic, payload, 0, 1, true);

            snprintf(payload, sizeof(payload),
                "{\"name\":\"Deye Leistung\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                "\"avty_t\":\"%s\",\"val_tpl\":\"{{ value_json.deye_power }}\","
                "\"min\":1000,\"max\":20000,\"step\":100,\"unit_of_meas\":\"W\","
                "\"mode\":\"slider\",\"uniq_id\":\"%s_deye_power\","
                "\"icon\":\"mdi:battery-charging\","
                "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
                s_t_pwr_cmd, s_t_state, s_t_avail, s_devid, s_devid);
            esp_mqtt_client_publish(s_client, num_topic, payload, 0, 1, true);
        }
    }

    ESP_LOGI(TAG, "published %u HA-discovery configs%s", (unsigned)N_SENS,
             s_cfg.deny_ctrl ? " (no controls: MQTT control is off)" : " + 2 controls");
}

/* ------------------------- energy manager --------------------------
 * Own state topic (<base>/energy), so the meter state above stays small and
 * keeps its 5-s cadence; the energy state changes per quarter hour at most
 * and goes out every 30 s. The price curve (today + tomorrow) is a separate
 * retained message, published only when new prices arrive, and attached as
 * attributes of the price sensor -- that is what HA chart cards read.
 * Controls (mode, charge power, ...) follow the same deny_ctrl rule as the
 * Deye controls: switched off means deleted from HA, not just ignored. */

static const char *em_mode_name(uint8_t m)
{
    return m == EM_MODE_AUTO ? "Automatik" : (m == EM_MODE_SHADOW ? "Nur planen" : "Aus");
}
static const char *em_act_name(uint8_t a)
{
    return a == EM_ACT_CHARGE ? "Laden" : (a == EM_ACT_HOLD ? "Halten" : "Normal");
}

#define EM_DEV "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}"

static void em_disc(const char *comp, const char *key, const char *body)
{
    char topic[128], payload[900];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/em_%s/config", comp, s_devid, key);
    if (!body) { esp_mqtt_client_publish(s_client, topic, "", 0, 1, true); return; }
    int n = snprintf(payload, sizeof(payload), "{%s,\"avty_t\":\"%s\",\"uniq_id\":\"%s_em_%s\"," EM_DEV "}",
                     body, s_t_avail, s_devid, key, s_devid);
    if (n >= (int)sizeof(payload)) { ESP_LOGE(TAG, "discovery em_%s too long", key); return; }
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
}

static void publish_em_discovery(void)
{
    if (!s_cfg.discovery || !s_client) return;
    char b[640];
    struct { const char *key, *name, *unit, *dc, *sc, *icon; } S[] = {
        { "price",      "Strompreis",              "ct/kWh", NULL,        "measurement", "mdi:currency-eur" },
        { "ren",        "Anteil Erneuerbare",      "%",      NULL,        "measurement", "mdi:leaf" },
        { "solar",      "Anteil Sonne",            "%",      NULL,        "measurement", "mdi:white-balance-sunny" },
        { "wind",       "Anteil Wind",             "%",      NULL,        "measurement", "mdi:wind-turbine" },
        { "pv_today",   "PV-Prognose heute",       "kWh",    "energy",    NULL,          "mdi:solar-power" },
        { "pv_tomorrow","PV-Prognose morgen",      "kWh",    "energy",    NULL,          "mdi:solar-power" },
        { "saving",     "Ersparnis laut Plan",     "EUR",    "monetary",  NULL,          "mdi:piggy-bank" },
        { "byd_soc",    "BYD SoC (Fronius)",       "%",      "battery",   "measurement", NULL },
    };
    for (unsigned i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        int o = snprintf(b, sizeof(b), "\"name\":\"%s\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.%s }}\","
                         "\"unit_of_meas\":\"%s\"", S[i].name, s_t_em, S[i].key, S[i].unit);
        if (S[i].dc)   o += snprintf(b + o, sizeof(b) - o, ",\"dev_cla\":\"%s\"", S[i].dc);
        if (S[i].sc)   o += snprintf(b + o, sizeof(b) - o, ",\"stat_cla\":\"%s\"", S[i].sc);
        if (S[i].icon) o += snprintf(b + o, sizeof(b) - o, ",\"icon\":\"%s\"", S[i].icon);
        if (!strcmp(S[i].key, "price"))
            snprintf(b + o, sizeof(b) - o, ",\"json_attr_t\":\"%s\"", s_t_em_prices);
        em_disc("sensor", S[i].key, b);
    }
    snprintf(b, sizeof(b), "\"name\":\"Plan jetzt\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.act }}\","
             "\"dev_cla\":\"enum\",\"options\":[\"Normal\",\"Halten\",\"Laden\"],\"icon\":\"mdi:calendar-clock\"", s_t_em);
    em_disc("sensor", "act", b);
    snprintf(b, sizeof(b), "\"name\":\"Plan Begründung\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.why }}\","
             "\"icon\":\"mdi:text\"", s_t_em);
    em_disc("sensor", "why", b);
    snprintf(b, sizeof(b), "\"name\":\"Nächstes Laden ab\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.next_from }}\","
             "\"dev_cla\":\"timestamp\"", s_t_em);
    em_disc("sensor", "next_from", b);
    snprintf(b, sizeof(b), "\"name\":\"Nächstes Laden bis\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.next_to }}\","
             "\"dev_cla\":\"timestamp\"", s_t_em);
    em_disc("sensor", "next_to", b);
    snprintf(b, sizeof(b), "\"name\":\"BYD-Entladung gesperrt\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.byd_blocked }}\","
             "\"pl_on\":\"1\",\"pl_off\":\"0\",\"icon\":\"mdi:battery-lock\"", s_t_em);
    em_disc("binary_sensor", "byd_blocked", b);
    snprintf(b, sizeof(b), "\"name\":\"BYD-Zwangsbetrieb bis\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.byd_force_until }}\","
             "\"dev_cla\":\"timestamp\",\"icon\":\"mdi:timer-sand\"", s_t_em);
    em_disc("sensor", "byd_force_until", b);
    snprintf(b, sizeof(b), "\"name\":\"BYD-Zwangsbetrieb zuletzt beendet\",\"stat_t\":\"%s\","
             "\"val_tpl\":\"{{ value_json.byd_force_end }}\",\"icon\":\"mdi:text\"", s_t_em);
    em_disc("sensor", "byd_force_end", b);
    snprintf(b, sizeof(b), "\"name\":\"Deye-Akku gesperrt bis\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.deye_lock_until }}\","
             "\"dev_cla\":\"timestamp\",\"icon\":\"mdi:battery-clock\"", s_t_em);
    em_disc("sensor", "deye_lock_until", b);
    snprintf(b, sizeof(b), "\"name\":\"BYD-Akku gesperrt bis\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.byd_lock_until }}\","
             "\"dev_cla\":\"timestamp\",\"icon\":\"mdi:battery-clock\"", s_t_em);
    em_disc("sensor", "byd_lock_until", b);

    /* Controls. */
    bool ctl = !s_cfg.deny_ctrl;
    char cmd[120];
#define CMD(k) (snprintf(cmd, sizeof(cmd), "%s/%s/set", s_t_em_cmd, k), cmd)
    if (ctl) {
        snprintf(b, sizeof(b), "\"name\":\"Energiemanager\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.mode }}\",\"options\":[\"Aus\",\"Nur planen\",\"Automatik\"],"
                 "\"icon\":\"mdi:home-lightning-bolt\"", CMD("mode"), s_t_em);
        em_disc("select", "mode", b);
        snprintf(b, sizeof(b), "\"name\":\"Netzladen Leistung\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.charge_kw }}\",\"min\":0.25,\"max\":20,\"step\":0.25,"
                 "\"unit_of_meas\":\"kW\",\"mode\":\"box\",\"icon\":\"mdi:battery-charging\"", CMD("charge_kw"), s_t_em);
        em_disc("number", "charge_kw", b);
        snprintf(b, sizeof(b), "\"name\":\"Netzladen bis SoC\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.soc_max }}\",\"min\":20,\"max\":100,\"step\":1,"
                 "\"unit_of_meas\":\"%%\",\"mode\":\"slider\",\"icon\":\"mdi:battery-arrow-up\"", CMD("soc_max"), s_t_em);
        em_disc("number", "soc_max", b);
        snprintf(b, sizeof(b), "\"name\":\"Mindestvorteil\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.spread }}\",\"min\":0,\"max\":50,\"step\":0.5,"
                 "\"unit_of_meas\":\"ct/kWh\",\"mode\":\"box\",\"icon\":\"mdi:scale-balance\"", CMD("spread"), s_t_em);
        em_disc("number", "spread", b);
        snprintf(b, sizeof(b), "\"name\":\"BYD-Steuerung erlaubt\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.byd_ctrl }}\",\"pl_on\":\"1\",\"pl_off\":\"0\","
                 "\"icon\":\"mdi:battery-lock-open\"", CMD("byd_ctrl"), s_t_em);
        em_disc("switch", "byd_ctrl", b);
        /* BYD forced operation: the numbers are the parameters, the select
         * starts/stops with them (needs byd_ctrl, like on the web page). */
        snprintf(b, sizeof(b), "\"name\":\"BYD-Zwangsbetrieb\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.byd_force }}\",\"options\":[\"Aus\",\"Laden\",\"Entladen\"],"
                 "\"icon\":\"mdi:battery-sync\"", CMD("byd_force"), s_t_em);
        em_disc("select", "byd_force", b);
        /* The battery locks, e.g. switched by an automation while the car
         * charges. Until lifted; a timed lock comes from the web page. */
        snprintf(b, sizeof(b), "\"name\":\"Deye-Akku sperren\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.deye_lock }}\",\"pl_on\":\"1\",\"pl_off\":\"0\","
                 "\"icon\":\"mdi:battery-lock\"", CMD("deye_lock"), s_t_em);
        em_disc("switch", "deye_lock", b);
        snprintf(b, sizeof(b), "\"name\":\"BYD-Akku sperren\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.byd_lock }}\",\"pl_on\":\"1\",\"pl_off\":\"0\","
                 "\"icon\":\"mdi:battery-lock\"", CMD("byd_lock"), s_t_em);
        em_disc("switch", "byd_lock", b);
        snprintf(b, sizeof(b), "\"name\":\"BYD-Zwangsbetrieb Leistung\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.byd_force_w }}\",\"min\":%d,\"max\":%d,\"step\":100,"
                 "\"unit_of_meas\":\"W\",\"mode\":\"box\",\"icon\":\"mdi:flash\"",
                 CMD("byd_force_w"), s_t_em, FRONIUS_FORCE_MIN_W, FRONIUS_FORCE_MAX_W);
        em_disc("number", "byd_force_w", b);
        snprintf(b, sizeof(b), "\"name\":\"BYD-Zwangsbetrieb Dauer\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.byd_force_min }}\",\"min\":1,\"max\":%d,\"step\":1,"
                 "\"unit_of_meas\":\"min\",\"mode\":\"box\",\"icon\":\"mdi:timer\"",
                 CMD("byd_force_min"), s_t_em, FRONIUS_FORCE_MAX_MIN);
        em_disc("number", "byd_force_min", b);
        snprintf(b, sizeof(b), "\"name\":\"BYD-Zwangsbetrieb bis SoC\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.byd_force_soc }}\",\"min\":0,\"max\":100,\"step\":1,"
                 "\"unit_of_meas\":\"%%\",\"mode\":\"slider\",\"icon\":\"mdi:battery-arrow-up-outline\"",
                 CMD("byd_force_soc"), s_t_em);
        em_disc("number", "byd_force_soc", b);
        /* The Deye's forced mode with time and SoC target, like the BYD's. */
        snprintf(b, sizeof(b), "\"name\":\"Deye-Zwangsbetrieb\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.deye_force }}\",\"options\":[\"Aus\",\"Laden\",\"Entladen\"],"
                 "\"icon\":\"mdi:battery-sync\"", CMD("deye_force"), s_t_em);
        em_disc("select", "deye_force", b);
        snprintf(b, sizeof(b), "\"name\":\"Deye-Zwangsbetrieb Leistung\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.deye_force_w }}\",\"min\":%d,\"max\":%d,\"step\":250,"
                 "\"unit_of_meas\":\"W\",\"mode\":\"box\",\"icon\":\"mdi:flash\"",
                 CMD("deye_force_w"), s_t_em, DEYE_FORCE_MIN_W, DEYE_POWER_MAX);
        em_disc("number", "deye_force_w", b);
        snprintf(b, sizeof(b), "\"name\":\"Deye-Zwangsbetrieb Dauer\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.deye_force_min }}\",\"min\":1,\"max\":%d,\"step\":1,"
                 "\"unit_of_meas\":\"min\",\"mode\":\"box\",\"icon\":\"mdi:timer\"",
                 CMD("deye_force_min"), s_t_em, DEYE_FORCE_MAX_MIN);
        em_disc("number", "deye_force_min", b);
        snprintf(b, sizeof(b), "\"name\":\"Deye-Zwangsbetrieb bis SoC\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.deye_force_soc }}\",\"min\":10,\"max\":100,\"step\":1,"
                 "\"unit_of_meas\":\"%%\",\"mode\":\"slider\",\"icon\":\"mdi:battery-arrow-up-outline\"",
                 CMD("deye_force_soc"), s_t_em);
        em_disc("number", "deye_force_soc", b);
        snprintf(b, sizeof(b), "\"name\":\"Preise neu holen\",\"cmd_t\":\"%s\",\"pl_prs\":\"prices\","
                 "\"icon\":\"mdi:refresh\"", CMD("refresh"));
        em_disc("button", "refresh_prices", b);
        snprintf(b, sizeof(b), "\"name\":\"Neu planen\",\"cmd_t\":\"%s\",\"pl_prs\":\"plan\","
                 "\"icon\":\"mdi:calendar-refresh\"", CMD("refresh"));
        em_disc("button", "replan", b);
    } else {
        em_disc("select", "mode", NULL);
        em_disc("number", "charge_kw", NULL);
        em_disc("number", "soc_max", NULL);
        em_disc("number", "spread", NULL);
        em_disc("switch", "byd_ctrl", NULL);
        em_disc("select", "byd_force", NULL);
        em_disc("switch", "deye_lock", NULL);
        em_disc("switch", "byd_lock", NULL);
        em_disc("number", "byd_force_w", NULL);
        em_disc("number", "byd_force_min", NULL);
        em_disc("number", "byd_force_soc", NULL);
        em_disc("select", "deye_force", NULL);
        em_disc("number", "deye_force_w", NULL);
        em_disc("number", "deye_force_min", NULL);
        em_disc("number", "deye_force_soc", NULL);
        em_disc("button", "refresh_prices", NULL);
        em_disc("button", "replan", NULL);
    }
#undef CMD
}

/* NAN -> JSON null; HA shows "unknown" instead of a made-up 0. */
static int jf(char *b, size_t cap, int o, const char *key, float v, int dec)
{
    if (o < 0 || (size_t)o >= cap) return o;
    int n = isnan(v) ? snprintf(b + o, cap - o, ",\"%s\":null", key)
                     : snprintf(b + o, cap - o, ",\"%s\":%.*f", key, dec, v);
    return n < 0 ? o : o + n;
}

static void iso(char *b, size_t n, int64_t t)
{
    if (!t) { snprintf(b, n, "null"); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(b, n, "\"%Y-%m-%dT%H:%M:%S%z\"", &tm);
}

/* Parameters for the BYD's forced operation from Home Assistant. RAM only:
 * a forced mode is never resumed after a restart either. */
static struct { uint16_t w, min; uint8_t soc; } s_ff = { 2000, 60, 80 };
static struct { uint16_t w, min; uint8_t soc; } s_df = { 3000, 60, 80 };   /* the Deye's */

static void publish_em_state(void)
{
    if (!s_client || !s_st.connected) return;
    static em_status_t st;
    static em_cfg_t c;
    static EXT_RAM_BSS_ATTR fronius_storage_status_t fs;
    em_mgr_get_status(&st);
    em_cfg_get(&c);
    fronius_storage_get(&fs);
    static EXT_RAM_BSS_ATTR char j[1600];
    char nf[40], nt[40], why[160];
    iso(nf, sizeof(nf), st.next_charge_from);
    iso(nt, sizeof(nt), st.next_charge_to);
    /* the reason may hold quotes from a feed error */
    size_t k = 0;
    for (const char *s = st.why; *s && k + 2 < sizeof(why); s++) {
        if (*s == '"' || *s == '\\') why[k++] = '\\';
        why[k++] = *s;
    }
    why[k] = '\0';
    int o = snprintf(j, sizeof(j), "{\"mode\":\"%s\",\"act\":\"%s\",\"why\":\"%s\"",
                     em_mode_name(st.mode), em_act_name(st.act_now), why);
    o = jf(j, sizeof(j), o, "price", st.price_now_ct, 2);
    o = jf(j, sizeof(j), o, "ren", st.ren_now_pct, 1);
    o = jf(j, sizeof(j), o, "solar", st.solar_now_pct, 1);
    o = jf(j, sizeof(j), o, "wind", st.wind_now_pct, 1);
    o = jf(j, sizeof(j), o, "pv_today", st.pv_today_kwh, 2);
    o = jf(j, sizeof(j), o, "pv_tomorrow", st.pv_tomorrow_kwh, 2);
    o = jf(j, sizeof(j), o, "saving", st.saving_ct / 100.0f, 2);
    modbus_tcp_status_t ts;
    modbus_tcp_get_status(&ts);
    o = jf(j, sizeof(j), o, "byd_soc", fs.ok ? fs.soc_pct : (ts.byd_soc > 0.0f ? ts.byd_soc : NAN), 1);
    o = jf(j, sizeof(j), o, "charge_kw", c.charge_kw, 2);
    o = jf(j, sizeof(j), o, "spread", c.min_spread_ct, 1);
    char fu[40], lu[40], bu[40];
    iso(fu, sizeof(fu), fs.force ? fs.force_until : 0);
    deye_force_t dfs;
    deye_force_get(&dfs);
    bat_lock_t dl, bl;
    bat_lock_get(BAT_DEYE, &dl);
    bat_lock_get(BAT_BYD, &bl);
    iso(lu, sizeof(lu), dl.on ? dl.until : 0);
    iso(bu, sizeof(bu), bl.on ? bl.until : 0);
    if (o > 0 && (size_t)o < sizeof(j))
        o += snprintf(j + o, sizeof(j) - o, ",\"soc_max\":%u,\"byd_ctrl\":%u,\"byd_blocked\":%d,"
                      "\"next_from\":%s,\"next_to\":%s,\"byd_force\":\"%s\",\"byd_force_until\":%s,"
                      "\"byd_force_w\":%u,\"byd_force_min\":%u,\"byd_force_soc\":%u,\"byd_force_end\":\"%.47s\","
                      "\"deye_lock\":%d,\"deye_lock_until\":%s,\"byd_lock\":%d,\"byd_lock_until\":%s,"
                      "\"deye_force\":\"%s\",\"deye_force_w\":%u,\"deye_force_min\":%u,\"deye_force_soc\":%u}",
                      c.soc_max_pct, c.byd_ctrl, fs.blocked ? 1 : 0, nf, nt,
                      fs.force == FRONIUS_FORCE_CHARGE ? "Laden" : fs.force == FRONIUS_FORCE_DISCHARGE ? "Entladen" : "Aus",
                      fu, s_ff.w, s_ff.min, s_ff.soc, fs.force_end, dl.on ? 1 : 0, lu, bl.on ? 1 : 0, bu,
                      dfs.mode == DEYE_FORCE_CHARGE ? "Laden" : dfs.mode == DEYE_FORCE_DISCHARGE ? "Entladen" : "Aus",
                      s_df.w, s_df.min, s_df.soc);
    esp_mqtt_client_publish(s_client, s_t_em, j, 0, 0, s_cfg.retain);

    /* The price curve, only when it changed. */
    static int64_t sent_fetched = -1;
    static EXT_RAM_BSS_ATTR price_set_t ps;
    price_feed_get(&ps);
    if (ps.fetched != sent_fetched && ps.total.n) {
        static EXT_RAM_BSS_ATTR char pj[4096];
        int p = snprintf(pj, sizeof(pj), "{\"src\":\"%s\",\"t0\":%lld,\"step\":%d,\"total\":[",
                         ps.src, (long long)ps.total.t0, EM_SLOT_S);
        for (int i = 0; i < ps.total.n && p < (int)sizeof(pj) - 16; i++)
            p += isnan(ps.total.v[i]) ? snprintf(pj + p, sizeof(pj) - p, "%snull", i ? "," : "")
                                      : snprintf(pj + p, sizeof(pj) - p, "%s%.2f", i ? "," : "", ps.total.v[i]);
        snprintf(pj + p, sizeof(pj) - p, "]}");
        esp_mqtt_client_publish(s_client, s_t_em_prices, pj, 0, 1, true);
        sent_fetched = ps.fetched;
    }
}

/* <base>/em/byd_force[_w|_min|_soc]/set. A retained start is refused: the
 * broker would replay it on every reconnect and restart the forced mode. */
static void handle_byd_force(const char *sub, const char *d, bool retained)
{
    if (!*sub) {
        fronius_force_t m = !strcmp(d, "Laden") ? FRONIUS_FORCE_CHARGE
                          : !strcmp(d, "Entladen") ? FRONIUS_FORCE_DISCHARGE
                          : !strcmp(d, "Aus") ? FRONIUS_FORCE_OFF : (fronius_force_t)255;
        if ((int)m == 255) { ESP_LOGW(TAG, "MQTT byd_force '%s' unknown", d); return; }
        if (m == FRONIUS_FORCE_OFF) { fronius_storage_force_stop("über MQTT beendet"); return; }
        em_cfg_t c;
        em_cfg_get(&c);
        int64_t now = (int64_t)time(NULL);
        if (retained)           { ESP_LOGW(TAG, "MQTT byd_force '%s' retained -- ignored", d); return; }
        if (!c.byd_ctrl)        { ESP_LOGW(TAG, "MQTT byd_force refused: BYD control not allowed"); return; }
        if (now < 1700000000LL) { ESP_LOGW(TAG, "MQTT byd_force refused: clock not set"); return; }
        if (fronius_storage_force(m, s_ff.w, s_ff.min, s_ff.soc, now) != ESP_OK)
            ESP_LOGW(TAG, "MQTT byd_force refused (%u W, %u min, %u %%)", s_ff.w, s_ff.min, s_ff.soc);
        else
            em_mgr_want_fronius();
        return;
    }
    char *end = NULL;
    float v = strtof(d, &end);
    if (end == d || !isfinite(v)) { ESP_LOGW(TAG, "MQTT byd_force%s '%s' not a number", sub, d); return; }
    if (!strcmp(sub, "_w") && v >= FRONIUS_FORCE_MIN_W && v <= FRONIUS_FORCE_MAX_W) s_ff.w = (uint16_t)(v + 0.5f);
    else if (!strcmp(sub, "_min") && v >= 1 && v <= FRONIUS_FORCE_MAX_MIN)      s_ff.min = (uint16_t)(v + 0.5f);
    else if (!strcmp(sub, "_soc") && v >= 0 && v <= 100)                         s_ff.soc = (uint8_t)(v + 0.5f);
    else ESP_LOGW(TAG, "MQTT byd_force%s '%s' out of range", sub, d);
}

/* <base>/em/deye_force[_w|_min|_soc]/set -- as the BYD's above. */
static void handle_deye_force(const char *sub, const char *d, bool retained)
{
    if (!*sub) {
        deye_force_mode_t m = !strcmp(d, "Laden") ? DEYE_FORCE_CHARGE
                            : !strcmp(d, "Entladen") ? DEYE_FORCE_DISCHARGE
                            : !strcmp(d, "Aus") ? DEYE_FORCE_OFF : (deye_force_mode_t)255;
        if ((int)m == 255) { ESP_LOGW(TAG, "MQTT deye_force '%s' unknown", d); return; }
        if (m != DEYE_FORCE_OFF && retained) { ESP_LOGW(TAG, "MQTT deye_force '%s' retained -- ignored", d); return; }
        if (deye_force_set(m, s_df.w, s_df.min, s_df.soc, (int64_t)time(NULL)) != ESP_OK)
            ESP_LOGW(TAG, "MQTT deye_force refused (%u W, %u min, %u %%)", s_df.w, s_df.min, s_df.soc);
        return;
    }
    char *end = NULL;
    float v = strtof(d, &end);
    if (end == d || !isfinite(v)) { ESP_LOGW(TAG, "MQTT deye_force%s '%s' not a number", sub, d); return; }
    if (!strcmp(sub, "_w") && v >= DEYE_FORCE_MIN_W && v <= DEYE_POWER_MAX)   s_df.w = (uint16_t)(v + 0.5f);
    else if (!strcmp(sub, "_min") && v >= 1 && v <= DEYE_FORCE_MAX_MIN)      s_df.min = (uint16_t)(v + 0.5f);
    else if (!strcmp(sub, "_soc") && v >= 10 && v <= 100)                    s_df.soc = (uint8_t)(v + 0.5f);
    else ESP_LOGW(TAG, "MQTT deye_force%s '%s' out of range", sub, d);
}

/* <base>/em/<key>/set -> settings. Same strictness as the Deye commands:
 * anything unparsable is refused and the real state re-published. */
static void handle_em_cmd(const char *key, const char *d, bool retained)
{
    if (s_cfg.deny_ctrl) { ESP_LOGW(TAG, "MQTT em '%s' ignored -- control is off", key); return; }
    char err[100] = "";
    if (!strcmp(key, "refresh")) { em_mgr_request(d); return; }
    if (!strncmp(key, "byd_force", 9)) { handle_byd_force(key + 9, d, retained); return; }
    if (!strncmp(key, "deye_force", 10)) { handle_deye_force(key + 10, d, retained); return; }
    if (!strcmp(key, "deye_lock") || !strcmp(key, "byd_lock")) {
        /* A retained command would be replayed on every reconnect and undo
         * whatever was switched on the page since. */
        if (retained) { ESP_LOGW(TAG, "MQTT %s '%s' retained -- ignored", key, d); return; }
        bool on = !strcmp(d, "1") || !strcasecmp(d, "ON");
        bool off = !strcmp(d, "0") || !strcasecmp(d, "OFF");
        if (!on && !off) { ESP_LOGW(TAG, "MQTT %s '%s' unknown", key, d); return; }
        bool byd = key[0] == 'b';
        if (byd && on) {
            em_cfg_t c;
            em_cfg_get(&c);
            if (!c.byd_ctrl) { ESP_LOGW(TAG, "MQTT byd_lock refused: BYD control not allowed"); return; }
        }
        bat_lock_set(byd ? BAT_BYD : BAT_DEYE, on, 0, (int64_t)time(NULL));
        if (byd) em_mgr_want_fronius();
        return;
    }
    if (!strcmp(key, "mode")) {
        uint8_t m = !strcmp(d, "Automatik") ? EM_MODE_AUTO : !strcmp(d, "Nur planen") ? EM_MODE_SHADOW
                  : !strcmp(d, "Aus") ? EM_MODE_OFF : 255;
        if (m == 255) { ESP_LOGW(TAG, "MQTT em mode '%s' unknown", d); return; }
        em_mgr_set_mode(m, err, sizeof(err));
    } else {
        em_cfg_t c;
        em_cfg_get(&c);
        char *end = NULL;
        float v = strtof(d, &end);
        if (end == d || !isfinite(v)) { ESP_LOGW(TAG, "MQTT em %s '%s' not a number", key, d); return; }
        if      (!strcmp(key, "charge_kw")) c.charge_kw = v;
        else if (!strcmp(key, "soc_max"))   c.soc_max_pct = (v >= 0 && v <= 100) ? (uint8_t)(v + 0.5f) : 255;
        else if (!strcmp(key, "spread"))    c.min_spread_ct = v;
        else if (!strcmp(key, "byd_ctrl"))  c.byd_ctrl = v != 0.0f;
        else { ESP_LOGW(TAG, "MQTT em key '%s' unknown", key); return; }
        if (em_cfg_set(&c, err, sizeof(err)) == ESP_OK) em_mgr_request("plan");
    }
    if (err[0]) ESP_LOGW(TAG, "MQTT em %s '%s' refused: %s", key, d, err);
}

static void publish_state(void)
{
    if (!s_client || !s_st.connected) return;
    modbus_tcp_status_t st;
    modbus_tcp_get_status(&st);
    char j[320];
    snprintf(j, sizeof(j),
        "{\"pv_w\":%.0f,\"house_w\":%.0f,\"grid_w\":%.0f,"
        "\"byd_w\":%.0f,\"byd_soc\":%.0f,\"deye_w\":%.0f,\"deye_soc\":%.0f,"
        "\"deye_mode\":\"%s\",\"deye_power\":%d}",
        st.pv_w, st.house_w, st.grid_w, st.byd_w, st.byd_soc, st.deye_w, st.deye_soc,
        /* The HA number entity is the USER setpoint. Reporting the throttled
         * value made the slider jump every time the SLS guard stepped in. */
        deye_ha_mode(deye_ctrl_get_mode()), deye_ctrl_get_user_power());
    esp_mqtt_client_publish(s_client, s_t_state, j, 0, 0, s_cfg.retain);
    portENTER_CRITICAL(&s_mux); s_st.published++; portEXIT_CRITICAL(&s_mux);
}

/* Slot of the first enabled Eltako device per EN_ROLE entry, -1 = none. From
 * the live list (RAM), not modbus_tcp_get_devices(), which reads NVS. */
static void en_slots(int slot[N_EN_ROLE])
{
    mb_dev_live_t dl[MB_MAX_DEVICES];
    int n = modbus_tcp_get_device_live(dl, MB_MAX_DEVICES);
    for (unsigned r = 0; r < N_EN_ROLE; r++) {
        slot[r] = -1;
        for (int i = 0; i < n && slot[r] < 0; i++)
            if (dl[i].enabled && dl[i].mfr == MB_MFR_ELTAKO && dl[i].role == EN_ROLE[r].role)
                slot[r] = i;
    }
}

/* Roles whose energy sensors HA currently knows about, bit r = EN_ROLE[r];
 * -1 = not announced since this connection. */
static int s_en_disc = -1;

/* Announce the counter sensors of every role that has an Eltako meter, and
 * delete those of roles that have none (retained config, see the controls in
 * publish_discovery). Re-run whenever that set changes. */
static void publish_meter_discovery(int mask)
{
    if (!s_cfg.discovery || !s_client) return;
    for (unsigned r = 0; r < N_EN_ROLE; r++) {
        for (int k = 0; k < 2; k++) {
            const char *dir = k ? "export" : "import";
            char topic[128], payload[560];
            snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s_%s_kwh/config",
                     s_devid, EN_ROLE[r].key, dir);
            if (!(mask & (1 << r))) {
                esp_mqtt_client_publish(s_client, topic, "", 0, 1, true);
                continue;
            }
            snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\",\"stat_t\":\"%s/meter/%s\",\"avty_t\":\"%s\","
                "\"val_tpl\":\"{{ value_json.%s_kwh }}\",\"unit_of_meas\":\"kWh\","
                "\"dev_cla\":\"energy\",\"stat_cla\":\"total_increasing\","
                "\"sug_dsp_prc\":2,\"uniq_id\":\"%s_%s_%s_kwh\","
                "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
                k ? EN_ROLE[r].exp : EN_ROLE[r].imp, s_cfg.base, EN_ROLE[r].key, s_t_avail,
                dir, s_devid, EN_ROLE[r].key, dir, s_devid);
            esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
        }
    }
    s_en_disc = mask;
}

static void publish_meter_energy(void)
{
    if (!s_client || !s_st.connected) return;
    int slot[N_EN_ROLE], mask = 0;
    en_slots(slot);
    for (unsigned r = 0; r < N_EN_ROLE; r++) if (slot[r] >= 0) mask |= 1 << r;
    if (mask != s_en_disc) publish_meter_discovery(mask);

    for (unsigned r = 0; r < N_EN_ROLE; r++) {
        mb_energy_t e;
        if (slot[r] < 0 || !modbus_tcp_get_energy(slot[r], &e) || e.age_ms > EN_MAX_AGE_MS)
            continue;
        char topic[96], j[80];
        snprintf(topic, sizeof(topic), "%s/meter/%s", s_cfg.base, EN_ROLE[r].key);
        snprintf(j, sizeof(j), "{\"import_kwh\":%.2f,\"export_kwh\":%.2f}",
                 e.import_kwh, e.export_kwh);
        esp_mqtt_client_publish(s_client, topic, j, 0, 0, s_cfg.retain);
    }
}

/* Battery energy for HA's energy dashboard, one topic per battery:
 * <base>/battery/<key> = {"charge_kwh":..,"discharge_kwh":..}. The Deye's come
 * from its own counters (modbus_rtu.h), the BYD's are integrated on the
 * display (byd_energy.h). The SoC sensors already exist (deye_soc, byd_soc
 * in the state JSON). A battery's sensors are announced once it has
 * delivered values and are never deleted automatically: a Deye that misses a
 * poll round at boot must not make HA drop and re-create its entities. */
static const struct { const char *key, *chg, *dis; } BAT[] = {
    { "deye", "Deye Akku geladen", "Deye Akku entladen" },
    { "byd",  "BYD Akku geladen",  "BYD Akku entladen"  },
};
#define N_BAT (sizeof(BAT) / sizeof(BAT[0]))
/* The Deye counters stop being sent when older than this (bus outage). */
#define BAT_DEYE_MAX_AGE_MS (4 * MB_DEYE_ECTR_POLL_MS)

static int s_bat_disc;          /* bit b = BAT[b] announced on this connection */

static void publish_battery_discovery(unsigned b)
{
    for (int k = 0; k < 2; k++) {
        const char *dir = k ? "discharge" : "charge";
        char topic[128], payload[560];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s_bat_%s_kwh/config",
                 s_devid, BAT[b].key, dir);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"%s\",\"stat_t\":\"%s/battery/%s\",\"avty_t\":\"%s\","
            "\"val_tpl\":\"{{ value_json.%s_kwh }}\",\"unit_of_meas\":\"kWh\","
            "\"dev_cla\":\"energy\",\"stat_cla\":\"total_increasing\","
            "\"sug_dsp_prc\":1,\"uniq_id\":\"%s_%s_bat_%s_kwh\","
            "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
            k ? BAT[b].dis : BAT[b].chg, s_cfg.base, BAT[b].key, s_t_avail,
            dir, s_devid, BAT[b].key, dir, s_devid);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }
    s_bat_disc |= 1 << b;
}

/* Own frame, so the live snapshot is off the stack again before the
 * discovery payloads go on it. Not static: both the mqtt task and the
 * client's event handler publish. */
static bool deye_bat_energy(double *chg, double *dis)
{
    mb_deye_live_t l;
    modbus_rtu_get_deye_live(&l);
    *chg = l.bat_chg_kwh; *dis = l.bat_dis_kwh;
    return l.ectr_ok && l.ectr_age_ms <= BAT_DEYE_MAX_AGE_MS;
}

static void publish_battery_energy(void)
{
    if (!s_client || !s_st.connected) return;
    double chg[N_BAT], dis[N_BAT];
    bool have[N_BAT];
    have[0] = deye_bat_energy(&chg[0], &dis[0]);
    have[1] = byd_energy_get(&chg[1], &dis[1]);

    for (unsigned b = 0; b < N_BAT; b++) {
        if (!have[b]) continue;
        if (s_cfg.discovery && !(s_bat_disc & (1 << b))) publish_battery_discovery(b);
        char topic[96], j[96];
        snprintf(topic, sizeof(topic), "%s/battery/%s", s_cfg.base, BAT[b].key);
        snprintf(j, sizeof(j), "{\"charge_kwh\":%.3f,\"discharge_kwh\":%.3f}", chg[b], dis[b]);
        esp_mqtt_client_publish(s_client, topic, j, 0, 0, s_cfg.retain);
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)args; (void)base; (void)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        portENTER_CRITICAL(&s_mux); s_st.connected = true; portEXIT_CRITICAL(&s_mux);
        esp_mqtt_client_publish(s_client, s_t_avail, "online", 0, 1, true);
        if (!s_cfg.deny_ctrl) {
            esp_mqtt_client_subscribe(s_client, s_t_mode_cmd, 1);
            esp_mqtt_client_subscribe(s_client, s_t_pwr_cmd, 1);
            char t[112];
            snprintf(t, sizeof(t), "%s/+/set", s_t_em_cmd);
            esp_mqtt_client_subscribe(s_client, t, 1);
        } else {
            ESP_LOGW(TAG, "MQTT control is off -- not subscribing to the command topics");
        }
        publish_discovery();
        publish_em_discovery();
        s_en_disc = -1;                        /* announce the counters afresh */
        s_bat_disc = 0;
        publish_state();
        publish_em_state();
        publish_meter_energy();
        publish_battery_energy();
        break;
    case MQTT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&s_mux); s_st.connected = false; portEXIT_CRITICAL(&s_mux);
        break;
    case MQTT_EVENT_DATA: {
        esp_mqtt_event_handle_t ev = event_data;
        char d[24];
        int n = ev->data_len < (int)sizeof(d) - 1 ? ev->data_len : (int)sizeof(d) - 1;
        memcpy(d, ev->data, n); d[n] = '\0';

        bool is_mode = (ev->topic_len == (int)strlen(s_t_mode_cmd) &&
                        !strncmp(ev->topic, s_t_mode_cmd, ev->topic_len));
        bool is_pwr  = (ev->topic_len == (int)strlen(s_t_pwr_cmd) &&
                        !strncmp(ev->topic, s_t_pwr_cmd, ev->topic_len));

        /* <base>/em/<key>/set */
        size_t pl = strlen(s_t_em_cmd);
        if (ev->topic_len > (int)pl + 5 && !strncmp(ev->topic, s_t_em_cmd, pl) && ev->topic[pl] == '/' &&
            !strncmp(ev->topic + ev->topic_len - 4, "/set", 4)) {
            char key[24];
            int kl = ev->topic_len - (int)pl - 5;
            if (kl > 0 && kl < (int)sizeof(key)) {
                memcpy(key, ev->topic + pl + 1, kl); key[kl] = '\0';
                handle_em_cmd(key, d, ev->retain);
                publish_em_state();
            }
            break;
        }

        /* A belt to go with the braces above: a retained command from an
         * earlier session, or a subscription the broker still holds, must not
         * slip through after the switch was turned off. */
        if ((is_mode || is_pwr) && s_cfg.deny_ctrl) {
            ESP_LOGW(TAG, "MQTT command '%s' ignored -- control is switched off", d);
            publish_state();                   /* snap HA back to reality */
            break;
        }

        if (is_mode) {
            deye_mode_t m = deye_mode_from_ha(d);
            if (m == DEYE_MODE_COUNT) {
                ESP_LOGW(TAG, "MQTT mode cmd '%s' unknown -- ignored", d);
                publish_state();               /* re-assert the real state to HA */
                break;
            }
            ESP_LOGI(TAG, "MQTT mode cmd '%s' -> %s", d, deye_ctrl_mode_name(m));
            deye_ctrl_apply(m, deye_ctrl_get_user_power());
            publish_state();
        } else if (is_pwr) {
            /* atoi("abc") is 0, which the clamp turned into 1000 W and applied.
             * Parse strictly: digits only, within the advertised range. */
            char *end = NULL;
            long w = strtol(d, &end, 10);
            if (end == d || (*end != '\0' && *end != '.') ||
                w < DEYE_POWER_MIN || w > DEYE_POWER_MAX) {
                ESP_LOGW(TAG, "MQTT power cmd '%s' invalid (need %d..%d) -- ignored",
                         d, DEYE_POWER_MIN, DEYE_POWER_MAX);
                publish_state();
                break;
            }
            ESP_LOGI(TAG, "MQTT power cmd %ld W", w);
            deye_ctrl_apply(deye_ctrl_get_mode(), (int)w);
            publish_state();
        }
        break;
    }
    default:
        break;
    }
}

/* (Re)create the client from the current config. Runs ONLY on the mqtt task:
 * mqtt_fwd_set_cfg() used to call this on the LVGL task, which destroyed
 * s_client while publish_state() on this task could be between its NULL check
 * and the publish -- and esp_mqtt_client_stop() blocks the UI for the whole
 * network timeout when the broker is unreachable. */
static volatile bool s_restart;

static void mqtt_apply(void)
{
    if (s_client) {
        /* Retained "offline" before we vanish, or HA keeps showing the device
         * as available with frozen values -- the last will only fires for an
         * unclean disconnect, not for esp_mqtt_client_stop(). */
        if (s_st.connected) {
            esp_mqtt_client_publish(s_client, s_t_avail, "offline", 0, 1, true);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    portENTER_CRITICAL(&s_mux); s_st.connected = false; s_st.enabled = s_cfg.enabled; portEXIT_CRITICAL(&s_mux);

    if (!s_cfg.enabled || s_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "disabled / no broker");
        return;
    }

    snprintf(s_t_avail, sizeof(s_t_avail), "%s/availability", s_cfg.base);
    snprintf(s_t_state, sizeof(s_t_state), "%s/state", s_cfg.base);
    snprintf(s_t_mode_cmd, sizeof(s_t_mode_cmd), "%s/deye/mode/set", s_cfg.base);
    snprintf(s_t_pwr_cmd,  sizeof(s_t_pwr_cmd),  "%s/deye/power/set", s_cfg.base);
    snprintf(s_t_em,        sizeof(s_t_em),        "%s/energy", s_cfg.base);
    snprintf(s_t_em_prices, sizeof(s_t_em_prices), "%s/energy/prices", s_cfg.base);
    snprintf(s_t_em_cmd,    sizeof(s_t_em_cmd),    "%s/em", s_cfg.base);

    esp_mqtt_client_config_t mc = {
        .broker.address.hostname  = s_cfg.host,
        .broker.address.port      = s_cfg.port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
    };
    if (s_cfg.user[0]) mc.credentials.username = s_cfg.user;
    if (s_cfg.pass[0]) mc.credentials.authentication.password = s_cfg.pass;
    if (s_cfg.lastwill) {
        mc.session.last_will.topic   = s_t_avail;
        mc.session.last_will.msg     = "offline";
        mc.session.last_will.qos     = 1;
        mc.session.last_will.retain  = true;
    }

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) { ESP_LOGE(TAG, "init failed"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "client started -> %s:%u (base '%s')", s_cfg.host, s_cfg.port, s_cfg.base);
}

static void mqtt_task(void *arg)
{
    (void)arg;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_devid, sizeof(s_devid), "deyedisp_%02x%02x%02x", mac[3], mac[4], mac[5]);

    load_cfg();
    mqtt_apply();

    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_restart) {                  /* settings saved -> re-create here */
            s_restart = false;
            mqtt_apply();
            tick = 0;
        }
        if (++tick % 5 == 0) publish_state();
        if (tick % 10 == 0) {                         /* = MB_ENERGY_POLL_MS */
            publish_meter_energy();
            publish_battery_energy();
        }
        if (tick >= 30) { tick = 0; publish_em_state(); }
    }
}

/* ----------------------------- public -------------------------------- */

esp_err_t mqtt_fwd_start(void)
{
    if (xTaskCreate(mqtt_task, "mqtt", 6144, NULL, 4, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}

void mqtt_fwd_get_cfg(mqtt_cfg_t *out)
{
    if (!out) return;
    /* Lazy-load: the settings UI reads this at boot, before mqtt_fwd_start()
     * has run -- without this the tab would show empty fields and a Save would
     * wipe the stored broker. */
    if (!s_loaded) load_cfg();
    portENTER_CRITICAL(&s_mux); *out = s_cfg; portEXIT_CRITICAL(&s_mux);
}

esp_err_t mqtt_fwd_set_cfg(const mqtt_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    esp_err_t e = nvs_store_set_mqtt(cfg, sizeof(*cfg));   /* raw: "" stays unset */
    if (e == ESP_OK) {
        mqtt_cfg_t c = *cfg;
        normalize_cfg(&c);
        portENTER_CRITICAL(&s_mux); s_cfg = c; s_loaded = true; portEXIT_CRITICAL(&s_mux);
        s_restart = true;                 /* applied by the mqtt task, see mqtt_apply() */
    }
    return e;
}

void mqtt_fwd_get_status(mqtt_fwd_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux); *out = s_st; portEXIT_CRITICAL(&s_mux);
}

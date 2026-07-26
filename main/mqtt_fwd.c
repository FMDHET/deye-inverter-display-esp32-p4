#include "mqtt_fwd.h"
#include "modbus_tcp.h"
#include "nvs_store.h"
#include "deye_ctrl.h"

#include <stdlib.h>

#include <string.h>
#include <stdio.h>

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

static deye_mode_t deye_mode_from_ha(const char *s)
{
    if (!strcmp(s, "Laden"))    return DEYE_MODE_FORCE_CHARGE;
    if (!strcmp(s, "Entladen")) return DEYE_MODE_FORCE_DISCHARGE;
    return DEYE_MODE_NORMAL;
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

static void load_cfg(void)
{
    mqtt_cfg_t c;
    memset(&c, 0, sizeof(c));
    if (nvs_store_get_mqtt(&c, sizeof(c)) != ESP_OK) {
        snprintf(c.base, sizeof(c.base), "deye-display");
        c.port = 1883;
        c.retain = 1; c.discovery = 1; c.lastwill = 1;
    }
    if (c.port == 0)     c.port = 1883;
    if (c.base[0] == '\0') snprintf(c.base, sizeof(c.base), "deye-display");
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
     * Both report their current value from the shared state JSON. */
    {
        char topic[120], payload[560];
        snprintf(topic, sizeof(topic),
                 "homeassistant/select/%s/deye_mode/config", s_devid);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Deye Modus\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
            "\"avty_t\":\"%s\",\"val_tpl\":\"{{ value_json.deye_mode }}\","
            "\"options\":[\"Normal\",\"Laden\",\"Entladen\"],"
            "\"uniq_id\":\"%s_deye_mode\",\"icon\":\"mdi:home-battery\","
            "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
            s_t_mode_cmd, s_t_state, s_t_avail, s_devid, s_devid);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);

        snprintf(topic, sizeof(topic),
                 "homeassistant/number/%s/deye_power/config", s_devid);
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Deye Leistung\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
            "\"avty_t\":\"%s\",\"val_tpl\":\"{{ value_json.deye_power }}\","
            "\"min\":1000,\"max\":20000,\"step\":100,\"unit_of_meas\":\"W\","
            "\"mode\":\"slider\",\"uniq_id\":\"%s_deye_power\","
            "\"icon\":\"mdi:battery-charging\","
            "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Deye Display\",\"mdl\":\"ESP32-P4\",\"mf\":\"DIY\"}}",
            s_t_pwr_cmd, s_t_state, s_t_avail, s_devid, s_devid);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
    }

    ESP_LOGI(TAG, "published %u HA-discovery configs + 2 controls", (unsigned)N_SENS);
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
        deye_ha_mode(deye_ctrl_get_mode()), deye_ctrl_get_power());
    esp_mqtt_client_publish(s_client, s_t_state, j, 0, 0, s_cfg.retain);
    portENTER_CRITICAL(&s_mux); s_st.published++; portEXIT_CRITICAL(&s_mux);
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
        esp_mqtt_client_subscribe(s_client, s_t_mode_cmd, 1);
        esp_mqtt_client_subscribe(s_client, s_t_pwr_cmd, 1);
        publish_discovery();
        publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&s_mux); s_st.connected = false; portEXIT_CRITICAL(&s_mux);
        break;
    case MQTT_EVENT_DATA: {
        esp_mqtt_event_handle_t ev = event_data;
        char d[16];
        int n = ev->data_len < (int)sizeof(d) - 1 ? ev->data_len : (int)sizeof(d) - 1;
        memcpy(d, ev->data, n); d[n] = '\0';

        bool is_mode = (ev->topic_len == (int)strlen(s_t_mode_cmd) &&
                        !strncmp(ev->topic, s_t_mode_cmd, ev->topic_len));
        bool is_pwr  = (ev->topic_len == (int)strlen(s_t_pwr_cmd) &&
                        !strncmp(ev->topic, s_t_pwr_cmd, ev->topic_len));

        if (is_mode) {
            deye_mode_t m = deye_mode_from_ha(d);
            ESP_LOGI(TAG, "MQTT mode cmd '%s' -> %s", d, deye_ctrl_mode_name(m));
            deye_ctrl_apply(m, deye_ctrl_get_power());
            publish_state();
        } else if (is_pwr) {
            int w = atoi(d);
            ESP_LOGI(TAG, "MQTT power cmd %d W", w);
            deye_ctrl_apply(deye_ctrl_get_mode(), w);
            publish_state();
        }
        break;
    }
    default:
        break;
    }
}

/* (Re)create the client from the current config. */
static void mqtt_apply(void)
{
    if (s_client) {
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

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        publish_state();
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
    esp_err_t e = nvs_store_set_mqtt(cfg, sizeof(*cfg));
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_mux); s_cfg = *cfg; s_loaded = true; portEXIT_CRITICAL(&s_mux);
        mqtt_apply();
    }
    return e;
}

void mqtt_fwd_get_status(mqtt_fwd_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux); *out = s_st; portEXIT_CRITICAL(&s_mux);
}

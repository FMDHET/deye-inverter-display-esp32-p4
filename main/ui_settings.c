#include "ui_settings.h"
#include "wifi_mgr.h"
#include "display.h"
#include "lvgl_port.h"
#include "nvs_store.h"
#include "ui_flow.h"
#include "modbus_tcp.h"
#include "modbus_rtu.h"
#include "modbus_gw.h"
#include "mqtt_fwd.h"
#include "ntp_client.h"
#include "wg_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "fonts.h"
#include "lvgl.h"

static const char *TAG = "ui_settings";

/* ----------------- Palette (same dark theme as ui_flow) ---------------- */
#define COL_BG         lv_color_hex(0x000000)
#define COL_PANEL      lv_color_hex(0x1c1f24)
#define COL_PANEL2     lv_color_hex(0x2a2e35)
#define COL_TEXT       lv_color_hex(0xe5e7eb)
#define COL_SUB        lv_color_hex(0x9aa0a8)
#define COL_ACCENT     lv_color_hex(0x2563eb)   /* darker blue for buttons/accents */
#define COL_OK         lv_color_hex(0x4cd97b)
#define COL_ON_OK      lv_color_hex(0x06210f)   /* near-black text on green buttons */
#define COL_WARN       lv_color_hex(0xf5c842)

/* ----------------- Layout constants ----------------------------------- */
#define BAR_H          56
#define TAB_W          220
#define TAB_H          52     /* 8 tabs * 52 = 416 <= rail height (424) */

#define ARRAY_LEN(a)   ((int)(sizeof(a) / sizeof((a)[0])))

typedef enum { TAB_WIFI = 0, TAB_DISPLAY, TAB_MBTCP, TAB_MBRTU, TAB_MQTT, TAB_ZEIT, TAB_VPN, TAB_SYSTEM, TAB_COUNT } tab_id_t;

static lv_obj_t       *s_screen;
static lv_obj_t       *s_main_screen;       /* the flow screen we return to */
static lv_obj_t       *s_tab_buttons[TAB_COUNT];
static lv_obj_t       *s_tab_pages[TAB_COUNT];
static tab_id_t        s_active_tab;

/* WiFi tab widgets */
static lv_obj_t       *s_wifi_status_lbl;
static lv_obj_t       *s_wifi_ssid_lbl;
static lv_obj_t       *s_wifi_ip_lbl;
static lv_obj_t       *s_wifi_ap_lbl;
static lv_obj_t       *s_wifi_list;        /* scan results (add new)     */
static lv_obj_t       *s_wifi_saved_list;  /* saved networks (connect/x) */

/* Password dialog */
static lv_obj_t       *s_pwd_dialog;
static lv_obj_t       *s_pwd_ta;
static lv_obj_t       *s_pwd_kbd;
static char            s_pending_ssid[33];

static lv_timer_t     *s_refresh_timer;

/* Display tab */
static lv_obj_t       *s_bright_val;
static lv_obj_t       *s_contrast_val;
static lv_obj_t       *s_sleep_dd;
static lv_obj_t       *s_orient_dd;
static lv_obj_t       *s_disp_status;

/* Modbus-TCP tab: device list + edit dialog */
static lv_obj_t       *s_mb_status;
static lv_obj_t       *s_mb_add_btn;     /* "+ Gerät" lives in the top bar */
static lv_obj_t       *s_wifi_scan_bar;  /* "Scan" lives in the top bar (WLAN tab) */
static lv_obj_t       *s_save_bar;       /* "Speichern" in the top bar (Display/RTU/MQTT) */
static lv_obj_t       *s_mb_list;
static lv_obj_t       *s_mb_dialog;
static lv_obj_t       *s_mb_mfr_dd;
static lv_obj_t       *s_mb_role_dd;
static lv_obj_t       *s_mb_en;
static lv_obj_t       *s_mb_name;
static lv_obj_t       *s_mb_ip;
static lv_obj_t       *s_mb_port;
static lv_obj_t       *s_mb_unit;
static lv_obj_t       *s_mb_poll;
static lv_obj_t       *s_mb_tmo;
static lv_obj_t       *s_mb_kbd;
static int             s_mb_edit_idx;
static int             s_mb_devn;
static mb_dev_cfg_t    s_mb_devs[MB_MAX_DEVICES];

/* Modbus-RTU tab: per-bus switch + role + slave-id + baud */
static lv_obj_t       *s_rtu_status;
static lv_obj_t       *s_rtu_sw[MB_RTU_BUSES];
static lv_obj_t       *s_rtu_role[MB_RTU_BUSES];
static lv_obj_t       *s_rtu_id[MB_RTU_BUSES];
static lv_obj_t       *s_rtu_baud[MB_RTU_BUSES];
static lv_obj_t       *s_rtu_selftest_lbl;
static lv_obj_t       *s_gw_sw;
static lv_obj_t       *s_gw_port;
static lv_obj_t       *s_gw_bus[MB_RTU_BUSES];
static lv_obj_t       *s_gw_status;

/* MQTT tab */
static lv_obj_t       *s_mqtt_status, *s_mqtt_kbd;
static lv_obj_t       *s_mqtt_en, *s_mqtt_host, *s_mqtt_port, *s_mqtt_user, *s_mqtt_pass, *s_mqtt_base;
static lv_obj_t       *s_mqtt_retain, *s_mqtt_disc, *s_mqtt_lwt;

/* Zeit / NTP tab */
static lv_obj_t       *s_ntp_status, *s_ntp_kbd;
static lv_obj_t       *s_ntp_en, *s_ntp_server, *s_ntp_tz;

/* VPN / WireGuard tab */
static lv_obj_t       *s_vpn_status, *s_vpn_kbd, *s_vpn_en;

/* System tab */
static lv_obj_t       *s_sls_dd;
static lv_obj_t       *s_sls_status_lbl;
static lv_obj_t       *s_vpn_privkey, *s_vpn_pubkey, *s_vpn_psk, *s_vpn_endpoint;
static lv_obj_t       *s_vpn_port, *s_vpn_addr, *s_vpn_mask, *s_vpn_keep;

/* Text field that receives physical-keyboard keys forwarded from the web UI. */
static lv_obj_t       *s_active_ta;

/* ------------------- forward decls ----------------------------------- */
static void tab_select(tab_id_t id);
static void wifi_refresh(void);
static void wifi_saved_refresh(void);
static void open_pwd_dialog(const char *ssid);
static void close_pwd_dialog(void);

/* ------------------- shared widget helpers ---------------------------- */

static lv_obj_t *make_checkbox(lv_obj_t *parent, const char *txt, int x, int y, bool on)
{
    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, txt);
    lv_obj_align(cb, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_text_color(cb, COL_TEXT, 0);
    if (on) lv_obj_add_state(cb, LV_STATE_CHECKED);
    return cb;
}

/* Small wrapping sub-label -- status lines and hints under a tab's controls. */
static lv_obj_t *wrap_label(lv_obj_t *parent, int32_t w, int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, F_SM, 0);
    lv_obj_set_style_text_color(l, COL_SUB, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_label_set_text(l, txt);
    return l;
}

/* Index of `v` in a small options table, or `dflt` if it is not listed. */
static int opt_idx(const uint32_t *tab, int n, uint32_t v, int dflt)
{
    for (int i = 0; i < n; i++) if (tab[i] == v) return i;
    return dflt;
}

/* ------------------- event handlers ---------------------------------- */

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    s_active_ta = NULL;
    if (s_refresh_timer) lv_timer_pause(s_refresh_timer);
    lv_screen_load_anim(s_main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                        220, 0, false);
}

static void tab_btn_cb(lv_event_t *e)
{
    tab_id_t id = (tab_id_t)(intptr_t)lv_event_get_user_data(e);
    tab_select(id);
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_clean(s_wifi_list);
    lv_obj_t *busy = lv_label_create(s_wifi_list);
    lv_label_set_text(busy, "scanne...");
    lv_obj_set_style_text_color(busy, COL_SUB, 0);
    esp_err_t err = wifi_mgr_scan_start();
    if (err != ESP_OK) {
        lv_label_set_text(busy, "Scan fehlgeschlagen");
    }
}

static void ssid_btn_cb(lv_event_t *e)
{
    const char *ssid = lv_event_get_user_data(e);
    open_pwd_dialog(ssid);
}

static void pwd_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *psk = lv_textarea_get_text(s_pwd_ta);
    ESP_LOGI(TAG, "Connect attempt to %s", s_pending_ssid);
    wifi_mgr_set_sta(s_pending_ssid, psk);
    close_pwd_dialog();
}

static void pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    close_pwd_dialog();
}

static void kbd_ready_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        pwd_connect_cb(NULL);
    } else if (code == LV_EVENT_CANCEL) {
        close_pwd_dialog();
    }
}

/* ------------------- saved networks (multi-credential) --------------- */

static void saved_connect_cb(lv_event_t *e)
{
    const char *ssid = lv_event_get_user_data(e);
    if (ssid && ssid[0]) wifi_mgr_connect_saved(ssid);
}

static void saved_refresh_async(void *p)
{
    (void)p;
    wifi_saved_refresh();
}

static void saved_remove_cb(lv_event_t *e)
{
    const char *ssid = lv_event_get_user_data(e);
    if (ssid && ssid[0]) wifi_mgr_remove_cred(ssid);
    /* Rebuild AFTER this event finishes -- we'd otherwise free the very button
     * that is currently handling the click. */
    lv_async_call(saved_refresh_async, NULL);
}

/* Rebuild the saved-networks list. Cheap signature check avoids flicker (and
 * scroll reset) when nothing changed -- it's called from the refresh timer. */
static void wifi_saved_refresh(void)
{
    if (!s_wifi_saved_list) return;

    /* All sizable buffers are static -- this runs only on the LVGL task, and
     * keeping them off the stack avoids overflowing it (settings crash fix). */
    static char        saved_ssids[WIFI_MGR_MAX_CREDS][33];
    static wifi_cred_t creds[WIFI_MGR_MAX_CREDS];
    int                n = wifi_mgr_get_creds(creds, WIFI_MGR_MAX_CREDS);
    char               active[33];
    wifi_mgr_get_active_ssid(active, sizeof(active));

    static char sig_prev[420];
    static char sig[420];
    int o = snprintf(sig, sizeof(sig), "%d|%s|", n, active);
    for (int i = 0; i < n && o < (int)sizeof(sig); i++)
        o += snprintf(sig + o, sizeof(sig) - o, "%s,", creds[i].ssid);
    if (strcmp(sig, sig_prev) == 0) return;
    strncpy(sig_prev, sig, sizeof(sig_prev) - 1);
    sig_prev[sizeof(sig_prev) - 1] = '\0';

    lv_obj_clean(s_wifi_saved_list);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "Gespeicherte Netze (%d/%d)", n, WIFI_MGR_MAX_CREDS);
    lv_list_add_text(s_wifi_saved_list, hdr);

    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_wifi_saved_list);
        lv_label_set_text(l, "keine -- unten ein Netz waehlen");
        lv_obj_set_style_text_color(l, COL_SUB, 0);
        lv_obj_set_style_pad_left(l, 10, 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        strncpy(saved_ssids[i], creds[i].ssid, 32);
        saved_ssids[i][32] = '\0';
        bool is_active = (active[0] && strcmp(active, creds[i].ssid) == 0);

        char line[40];
        snprintf(line, sizeof(line), "%s%s", creds[i].ssid, is_active ? "  " LV_SYMBOL_OK : "");
        lv_obj_t *btn = lv_list_add_button(s_wifi_saved_list, LV_SYMBOL_WIFI, line);
        /* Slim rows: small font + tight vertical padding. */
        lv_obj_set_style_text_font(btn, F_SM, 0);
        lv_obj_set_style_pad_top(btn, 5, 0);
        lv_obj_set_style_pad_bottom(btn, 5, 0);
        lv_obj_set_style_pad_right(btn, 30, 0);   /* room for the trash icon */
        lv_obj_add_event_cb(btn, saved_connect_cb, LV_EVENT_CLICKED, saved_ssids[i]);
        if (is_active) lv_obj_set_style_text_color(btn, COL_OK, 0);

        /* Flat, borderless trash icon on the right -- no box, so it doesn't
         * inflate the row. Enlarged hit area keeps it easy to tap. */
        lv_obj_t *del = lv_label_create(btn);
        lv_label_set_text(del, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(del, COL_SUB, 0);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(del, 16);
        lv_obj_add_event_cb(del, saved_remove_cb, LV_EVENT_CLICKED, saved_ssids[i]);
    }
}

/* ------------------- WiFi tab content -------------------------------- */

static void wifi_refresh(void)
{
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);

    const char *state_str;
    lv_color_t  state_col;
    switch (st.state) {
    case WIFI_MGR_BOOTING:        state_str = "Startet ..."; state_col = COL_SUB;  break;
    case WIFI_MGR_STA_CONNECTING: state_str = "Verbinde";    state_col = COL_WARN; break;
    case WIFI_MGR_STA_CONNECTED:  state_str = "Verbunden";   state_col = COL_OK;   break;
    case WIFI_MGR_AP_FALLBACK:    state_str = "AP Fallback"; state_col = COL_WARN; break;
    case WIFI_MGR_AP_ONLY:        state_str = "AP only";     state_col = COL_WARN; break;
    default:                      state_str = "?";           state_col = COL_SUB;  break;
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "Status: %s", state_str);
    lv_label_set_text(s_wifi_status_lbl, buf);
    lv_obj_set_style_text_color(s_wifi_status_lbl, state_col, 0);

    if (st.sta_ssid[0]) {
        snprintf(buf, sizeof(buf), "STA  %s  (%d dBm)",
                 st.sta_ssid, st.rssi);
    } else {
        snprintf(buf, sizeof(buf), "STA  -");
    }
    lv_label_set_text(s_wifi_ssid_lbl, buf);

    snprintf(buf, sizeof(buf), "IP  %s",
             st.sta_ip[0] ? st.sta_ip : "-");
    lv_label_set_text(s_wifi_ip_lbl, buf);

    if (st.ap_active) {
        snprintf(buf, sizeof(buf), "AP  %s @ %s",
                 st.ap_ssid, st.ap_ip);
    } else {
        snprintf(buf, sizeof(buf), "AP  aus");
    }
    lv_label_set_text(s_wifi_ap_lbl, buf);

    /* Populate scan list when finished. */
    if (!wifi_mgr_scan_busy()) {
        static size_t last_render_count = 0;
        static wifi_mgr_ap_t aps[16];   /* static: keep off the LVGL-task stack */
        size_t n = wifi_mgr_scan_results(aps, 16);
        if (n != last_render_count && n > 0) {
            lv_obj_clean(s_wifi_list);
            for (size_t i = 0; i < n; i++) {
                char line[64];
                snprintf(line, sizeof(line), "%s  (%d dBm)%s",
                         aps[i].ssid, aps[i].rssi,
                         aps[i].authmode == WIFI_AUTH_OPEN ? "  [open]" : "");
                lv_obj_t *btn = lv_list_add_button(s_wifi_list,
                                                  LV_SYMBOL_WIFI, line);
                static char ssid_storage[16][33];
                if (i < 16) {
                    strncpy(ssid_storage[i], aps[i].ssid, 32);
                    ssid_storage[i][32] = '\0';
                    lv_obj_add_event_cb(btn, ssid_btn_cb,
                                        LV_EVENT_CLICKED, ssid_storage[i]);
                }
            }
            last_render_count = n;
        }
    }

    wifi_saved_refresh();
}

static void wifi_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 16, 0);

    /* Status card */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), 104);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(card, COL_PANEL, 0);
    lv_obj_set_style_border_color(card, COL_PANEL2, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_status_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_wifi_status_lbl, F_MD, 0);
    lv_label_set_text(s_wifi_status_lbl, "Status: -");

    s_wifi_ssid_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_wifi_ssid_lbl, F_SM, 0);
    lv_obj_set_style_text_color(s_wifi_ssid_lbl, COL_SUB, 0);
    lv_label_set_text(s_wifi_ssid_lbl, "STA -");

    s_wifi_ip_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_wifi_ip_lbl, F_SM, 0);
    lv_obj_set_style_text_color(s_wifi_ip_lbl, COL_SUB, 0);
    lv_label_set_text(s_wifi_ip_lbl, "IP -");

    s_wifi_ap_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_wifi_ap_lbl, F_SM, 0);
    lv_obj_set_style_text_color(s_wifi_ap_lbl, COL_SUB, 0);
    lv_label_set_text(s_wifi_ap_lbl, "AP -");

    /* (Scan button lives in the top menu bar -- shown only on this tab.) */

    /* Saved networks (up to WIFI_MGR_MAX_CREDS): tap a row to connect, tap the
     * trash icon to forget it. Populated by wifi_saved_refresh(). */
    s_wifi_saved_list = lv_list_create(parent);
    lv_obj_set_size(s_wifi_saved_list, LV_PCT(100), 126);
    lv_obj_align(s_wifi_saved_list, LV_ALIGN_TOP_LEFT, 0, 112);
    lv_obj_set_style_bg_color(s_wifi_saved_list, COL_PANEL, 0);
    lv_obj_set_style_border_color(s_wifi_saved_list, COL_PANEL2, 0);
    lv_obj_set_style_border_width(s_wifi_saved_list, 1, 0);
    lv_obj_set_style_radius(s_wifi_saved_list, 10, 0);

    /* Scan results -- tap a network to add it (asks for the password). */
    s_wifi_list = lv_list_create(parent);
    lv_obj_set_size(s_wifi_list, LV_PCT(100), 168);
    lv_obj_align(s_wifi_list, LV_ALIGN_TOP_LEFT, 0, 248);
    lv_obj_set_style_bg_color(s_wifi_list, COL_PANEL, 0);
    lv_obj_set_style_border_color(s_wifi_list, COL_PANEL2, 0);
    lv_obj_set_style_border_width(s_wifi_list, 1, 0);
    lv_obj_set_style_radius(s_wifi_list, 10, 0);
    lv_obj_t *hint = lv_label_create(s_wifi_list);
    lv_label_set_text(hint, "Oben rechts \"Scan\" tippen, um Netzwerke zu suchen.");
    lv_obj_set_style_text_color(hint, COL_SUB, 0);

    wifi_saved_refresh();
}

/* ------------------- Display tab (brightness / contrast) ------------- */

/* Snap a slider value to a 5 % raster and write it back. */
static int slider_snap5(lv_obj_t *s)
{
    int v = ((lv_slider_get_value(s) + 2) / 5) * 5;
    if (v != lv_slider_get_value(s)) lv_slider_set_value(s, v, LV_ANIM_OFF);
    return v;
}
static void bright_change_cb(lv_event_t *e)
{
    int v = slider_snap5(lv_event_get_target(e));
    display_set_brightness((uint8_t)v);
    lv_label_set_text_fmt(s_bright_val, "%d %%", v);
}
static void bright_release_cb(lv_event_t *e)
{
    nvs_store_set_brightness((uint8_t)slider_snap5(lv_event_get_target(e)));
}
static void contrast_change_cb(lv_event_t *e)
{
    int v = slider_snap5(lv_event_get_target(e));
    ui_flow_set_contrast((uint8_t)v);
    lv_label_set_text_fmt(s_contrast_val, "%d %%", v);
}
static void contrast_release_cb(lv_event_t *e)
{
    nvs_store_set_contrast((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}

static lv_obj_t *labeled_slider(lv_obj_t *parent, const char *name, int val,
                                lv_obj_t **out_val,
                                lv_event_cb_t change_cb, lv_event_cb_t release_cb)
{
    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, name);
    lv_obj_set_style_text_font(hdr, F_MD, 0);
    lv_obj_set_style_text_color(hdr, COL_TEXT, 0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 30, 0);   /* clearance for the big knob */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_flex_grow(sl, 1);
    lv_obj_set_height(sl, 16);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, COL_PANEL2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_INDICATOR);
    /* Big round knob so it is easy to grab/drag. */
    lv_obj_set_style_bg_color(sl, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 14, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, change_cb,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, release_cb, LV_EVENT_RELEASED,      NULL);

    lv_obj_t *vl = lv_label_create(row);
    lv_label_set_text_fmt(vl, "%d %%", val);
    lv_obj_set_style_text_color(vl, COL_SUB, 0);
    lv_obj_set_width(vl, 72);
    lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
    *out_val = vl;
    return sl;
}

/* Standby dropdown options and their timeout in seconds (0 = off). */
static const uint16_t k_sleep_secs[] = { 0, 30, 60, 120, 300, 600 };
#define SLEEP_OPTS "Aus\n30 s\n1 min\n2 min\n5 min\n10 min"

/* Orientation dropdown options and their degrees of clockwise rotation.
 * Only the two landscape orientations are offered -- this HMI is laid out for
 * 800x480, so portrait (90/270) would just clip the layout and makes no sense. */
static const uint16_t k_orient_deg[] = { 0, 180 };
#define ORIENT_OPTS "Normal\n180\xC2\xB0 (umgedreht)"

static void sleep_save_cb(lv_event_t *e)
{
    (void)e;
    uint32_t sel = lv_dropdown_get_selected(s_sleep_dd);
    if (sel >= (uint32_t)ARRAY_LEN(k_sleep_secs)) sel = 0;
    uint16_t secs = k_sleep_secs[sel];

    nvs_store_set_sleep_secs(secs);
    ui_flow_set_sleep_timeout(secs);

    lv_label_set_text(s_disp_status, LV_SYMBOL_OK "  Gespeichert");
    lv_obj_set_style_text_color(s_disp_status, COL_OK, 0);
    ESP_LOGI(TAG, "standby timeout saved: %u s", (unsigned)secs);
}

/* Orientation applies AND persists the instant it is picked -- it must NOT
 * depend on the header Speichern button, because a portrait choice clips that
 * button off-screen and would trap the user with no way back. */
static void orient_change_cb(lv_event_t *e)
{
    (void)e;
    uint32_t osel = lv_dropdown_get_selected(s_orient_dd);
    if (osel >= (uint32_t)ARRAY_LEN(k_orient_deg)) osel = 0;
    uint8_t deg = (uint8_t)k_orient_deg[osel];

    nvs_store_set_orientation(deg);
    app_lvgl_apply_orientation(deg);

    lv_label_set_text(s_disp_status, LV_SYMBOL_OK "  Ausrichtung gespeichert");
    lv_obj_set_style_text_color(s_disp_status, COL_OK, 0);
    ESP_LOGI(TAG, "orientation changed + saved: %u deg", (unsigned)deg);
}

static void display_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 22, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(parent, 14, 0);

    labeled_slider(parent, "Helligkeit", nvs_store_get_brightness(),
                   &s_bright_val, bright_change_cb, bright_release_cb);
    labeled_slider(parent, "Kontrast", nvs_store_get_contrast(),
                   &s_contrast_val, contrast_change_cb, contrast_release_cb);

    /* Standby: turn the display off after N seconds of no touch. */
    lv_obj_t *sh = lv_label_create(parent);
    lv_label_set_text(sh, "Standby (Display aus nach)");
    lv_obj_set_style_text_font(sh, F_MD, 0);
    lv_obj_set_style_text_color(sh, COL_TEXT, 0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_sleep_dd = lv_dropdown_create(row);
    lv_dropdown_set_options(s_sleep_dd, SLEEP_OPTS);
    lv_obj_set_width(s_sleep_dd, 190);
    uint16_t saved = nvs_store_get_sleep_secs();
    for (int i = 0; i < ARRAY_LEN(k_sleep_secs); i++) {
        if (k_sleep_secs[i] == saved) {
            lv_dropdown_set_selected(s_sleep_dd, i);
            break;
        }
    }

    /* Orientation: rotate the image for a 90/180/270 deg mounting. */
    lv_obj_t *oh = lv_label_create(parent);
    lv_label_set_text(oh, "Ausrichtung");
    lv_obj_set_style_text_font(oh, F_MD, 0);
    lv_obj_set_style_text_color(oh, COL_TEXT, 0);

    lv_obj_t *orow = lv_obj_create(parent);
    lv_obj_remove_style_all(orow);
    lv_obj_set_width(orow, LV_PCT(100));
    lv_obj_set_height(orow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(orow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(orow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(orow, 16, 0);
    lv_obj_clear_flag(orow, LV_OBJ_FLAG_SCROLLABLE);

    s_orient_dd = lv_dropdown_create(orow);
    lv_dropdown_set_options(s_orient_dd, ORIENT_OPTS);
    lv_obj_set_width(s_orient_dd, 230);
    uint8_t cur_deg = nvs_store_get_orientation();
    for (int i = 0; i < ARRAY_LEN(k_orient_deg); i++) {
        if (k_orient_deg[i] == cur_deg) {
            lv_dropdown_set_selected(s_orient_dd, i);
            break;
        }
    }
    /* Apply + save immediately on selection (see orient_change_cb). */
    lv_obj_add_event_cb(s_orient_dd, orient_change_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Save button moved to the top menu bar (header_save_cb). */

    s_disp_status = lv_label_create(parent);
    lv_label_set_text(s_disp_status, "");
    lv_obj_set_style_text_font(s_disp_status, F_SM, 0);
    lv_obj_set_style_text_color(s_disp_status, COL_SUB, 0);
}

/* ------------------- Modbus-TCP tab ---------------------------------- */

static void mb_ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    s_active_ta = ta;                       /* receive PC-keyboard keys */
    /* Name takes free text; every other field is numeric. */
    lv_keyboard_set_mode(s_mb_kbd, ta == s_mb_name
                         ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_mb_kbd, ta);
    lv_obj_remove_flag(s_mb_kbd, LV_OBJ_FLAG_HIDDEN);
}

/* Physical keyboard forwarded from the web mirror -> focused text field. */
void ui_settings_web_key(uint32_t key)
{
    if (!s_active_ta) return;
    if (key == 8 || key == 127) {
        lv_textarea_delete_char(s_active_ta);
    } else if (key >= 0x20 && key < 0x7F) {
        lv_textarea_add_char(s_active_ta, key);
    }
}

void ui_settings_web_paste(const char *text)
{
    if (!s_active_ta || !text || !text[0]) return;
    lv_textarea_add_text(s_active_ta, text);   /* inserts the whole string */
}

void ui_settings_web_copy(char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = '\0';
    if (!s_active_ta) return;
    const char *t = lv_textarea_get_text(s_active_ta);
    if (t) {
        strncpy(out, t, outsz - 1);
        out[outsz - 1] = '\0';
    }
}

static void mb_kbd_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_mb_kbd, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mb_refresh(void)
{
    if (!s_mb_status) return;
    modbus_tcp_status_t st;
    modbus_tcp_get_status(&st);

    char buf[200];
    snprintf(buf, sizeof(buf),
             "%u/%u verbunden   (Polls %u, Fehler %u)\n"
             "PV %.0f W  Haus %.0f W  Netz %.0f W  Deye %.0f W (%.0f%%)  BYD %.0f W (%.0f%%)",
             st.connected, st.dev_count, (unsigned)st.poll_count, (unsigned)st.err_count,
             st.pv_w, st.house_w, st.grid_w, st.deye_w, st.deye_soc, st.byd_w, st.byd_soc);
    lv_label_set_text(s_mb_status, buf);
    lv_obj_set_style_text_color(s_mb_status,
        st.connected ? COL_OK : (st.dev_count ? COL_WARN : COL_SUB), 0);
}

static lv_obj_t *mb_textarea(lv_obj_t *parent, const char *placeholder,
                             const char *val, int w)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    if (val && val[0]) lv_textarea_set_text(ta, val);
    lv_obj_set_width(ta, w);
    lv_obj_set_style_bg_color(ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_add_event_cb(ta, mb_ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

/* forward decls (mutual references) */
static void open_dev_dialog(int idx);
static void dev_item_cb(lv_event_t *e);

static void mb_rebuild_list(void)
{
    if (!s_mb_list) return;
    lv_obj_clean(s_mb_list);
    s_mb_devn = modbus_tcp_get_devices(s_mb_devs, MB_MAX_DEVICES);
    for (int i = 0; i < s_mb_devn; i++) {
        char line[128];
        const char *nm = s_mb_devs[i].name[0] ? s_mb_devs[i].name : NULL;
        if (nm) {
            snprintf(line, sizeof(line), "%s%s\n%s - %s  %s",
                     nm, s_mb_devs[i].enabled ? "" : "  [aus]",
                     modbus_tcp_mfr_name(s_mb_devs[i].mfr),
                     modbus_tcp_role_name(s_mb_devs[i].role),
                     s_mb_devs[i].ip[0] ? s_mb_devs[i].ip : "(keine IP)");
        } else {
            snprintf(line, sizeof(line), "%s - %s\n%s%s",
                     modbus_tcp_mfr_name(s_mb_devs[i].mfr),
                     modbus_tcp_role_name(s_mb_devs[i].role),
                     s_mb_devs[i].ip[0] ? s_mb_devs[i].ip : "(keine IP)",
                     s_mb_devs[i].enabled ? "" : "  [aus]");
        }
        lv_obj_t *b = lv_list_add_button(s_mb_list, LV_SYMBOL_EDIT, line);
        lv_obj_add_event_cb(b, dev_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s_mb_devn == 0) {
        lv_obj_t *l = lv_label_create(s_mb_list);
        lv_label_set_text(l, "Noch keine Geräte - '+ Gerät' antippen.");
        lv_obj_set_style_text_color(l, COL_SUB, 0);
    }
}

static void dev_dialog_close(void)
{
    if (s_mb_dialog) {
        lv_obj_del(s_mb_dialog);
        s_mb_dialog = NULL;
        s_mb_kbd    = NULL;
        s_active_ta = NULL;
    }
}

static void dev_cancel_cb(lv_event_t *e) { (void)e; dev_dialog_close(); }

static void dev_save_cb(lv_event_t *e)
{
    (void)e;
    mb_dev_cfg_t c;
    memset(&c, 0, sizeof(c));
    strncpy(c.name, lv_textarea_get_text(s_mb_name), sizeof(c.name) - 1);
    strncpy(c.ip, lv_textarea_get_text(s_mb_ip), sizeof(c.ip) - 1);
    int port  = atoi(lv_textarea_get_text(s_mb_port));
    int slave = atoi(lv_textarea_get_text(s_mb_unit));
    int poll  = atoi(lv_textarea_get_text(s_mb_poll));
    int tmo   = atoi(lv_textarea_get_text(s_mb_tmo));
    c.port    = (port > 0 && port <= 65535) ? (uint16_t)port : 502;
    c.slave   = (slave >= 0 && slave <= 247) ? (uint8_t)slave : 1;
    if (poll <= 0) poll = MB_DEFAULT_POLL_MS;
    if (poll < MB_MIN_POLL_MS) poll = MB_MIN_POLL_MS;
    if (poll > MB_MAX_POLL_MS) poll = MB_MAX_POLL_MS;
    c.poll_ms = (uint16_t)poll;
    if (tmo <= 0) tmo = MB_DEFAULT_TIMEOUT_MS;
    if (tmo < MB_MIN_TIMEOUT_MS) tmo = MB_MIN_TIMEOUT_MS;
    if (tmo > MB_MAX_TIMEOUT_MS) tmo = MB_MAX_TIMEOUT_MS;
    c.timeout_ms = (uint16_t)tmo;
    c.mfr     = (uint8_t)lv_dropdown_get_selected(s_mb_mfr_dd);
    c.role    = (uint8_t)lv_dropdown_get_selected(s_mb_role_dd);
    c.enabled = lv_obj_has_state(s_mb_en, LV_STATE_CHECKED) ? 1 : 0;

    if (s_mb_edit_idx >= 0 && s_mb_edit_idx < s_mb_devn) {
        s_mb_devs[s_mb_edit_idx] = c;
    } else if (s_mb_devn < MB_MAX_DEVICES) {
        s_mb_devs[s_mb_devn++] = c;
    }
    modbus_tcp_set_devices(s_mb_devs, s_mb_devn);
    dev_dialog_close();
    mb_rebuild_list();
}

static void dev_del_cb(lv_event_t *e)
{
    (void)e;
    if (s_mb_edit_idx >= 0 && s_mb_edit_idx < s_mb_devn) {
        for (int i = s_mb_edit_idx; i < s_mb_devn - 1; i++) s_mb_devs[i] = s_mb_devs[i + 1];
        s_mb_devn--;
        modbus_tcp_set_devices(s_mb_devs, s_mb_devn);
    }
    dev_dialog_close();
    mb_rebuild_list();
}

/* German QWERTZ keyboard maps for the device name field.
 * Replaces TEXT_LOWER/TEXT_UPPER so ä/ö/ü/ß are reachable. */
static const char * const s_kb_de_lower[] = {
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "y", "x", "c", "v", "b", "n", "m", ".", ",", LV_SYMBOL_OK, "\n",
    "\xc3\xa4", "\xc3\xb6", "\xc3\xbc", "\xc3\x9f", " ", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "\n",
    NULL
};
static const lv_btnmatrix_ctrl_t s_kb_ctrl_de_lower[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, LV_BTNMATRIX_CTRL_POPOVER | 8,
    4, 4, 4, 4, 4, 4, 4, 4, 4,
    LV_BTNMATRIX_CTRL_CHECKABLE | 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, LV_BTNMATRIX_CTRL_POPOVER | 8,
    6, 6, 6, 6, 24, 6, 6
};
static const char * const s_kb_de_upper[] = {
    "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Y", "X", "C", "V", "B", "N", "M", ".", ",", LV_SYMBOL_OK, "\n",
    "\xc3\x84", "\xc3\x96", "\xc3\x9c", "\xc3\x9f", " ", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "\n",
    NULL
};
static const lv_btnmatrix_ctrl_t s_kb_ctrl_de_upper[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, LV_BTNMATRIX_CTRL_POPOVER | 8,
    4, 4, 4, 4, 4, 4, 4, 4, 4,
    LV_BTNMATRIX_CTRL_CHECKABLE | 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, LV_BTNMATRIX_CTRL_POPOVER | 8,
    6, 6, 6, 6, 24, 6, 6
};

static void open_dev_dialog(int idx)
{
    s_mb_edit_idx = idx;
    mb_dev_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.port = 502; c.slave = 1; c.enabled = 1;
    c.poll_ms = MB_DEFAULT_POLL_MS; c.timeout_ms = MB_DEFAULT_TIMEOUT_MS;
    if (idx >= 0 && idx < s_mb_devn) c = s_mb_devs[idx];

    s_mb_dialog = lv_obj_create(s_screen);
    lv_obj_set_size(s_mb_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_mb_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_mb_dialog, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_mb_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mb_dialog, 0, 0);
    lv_obj_set_style_radius(s_mb_dialog, 0, 0);
    /* No padding: it's a real full-screen page, and the default obj padding
     * was nudging the bottom-docked keyboard up over the red Löschen button. */
    lv_obj_set_style_pad_all(s_mb_dialog, 0, 0);
    lv_obj_clear_flag(s_mb_dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar, consistent with the main settings screen: Zurück (left) +
     * title (center) + Speichern (right). Keeps the save action always in the
     * same spot instead of buried in the dialog body. */
    lv_obj_t *dbar = lv_obj_create(s_mb_dialog);
    lv_obj_set_size(dbar, LV_PCT(100), BAR_H);
    lv_obj_set_pos(dbar, 0, 0);
    lv_obj_set_style_bg_color(dbar, COL_PANEL, 0);
    lv_obj_set_style_border_width(dbar, 0, 0);
    lv_obj_set_style_radius(dbar, 0, 0);
    lv_obj_set_style_pad_all(dbar, 0, 0);
    lv_obj_clear_flag(dbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dback = lv_button_create(dbar);
    lv_obj_set_size(dback, 140, 44);
    lv_obj_align(dback, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(dback, COL_PANEL2, 0);
    lv_obj_add_event_cb(dback, dev_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dbl = lv_label_create(dback);
    lv_label_set_text(dbl, LV_SYMBOL_LEFT "  Zurück"); lv_obj_center(dbl);

    lv_obj_t *title = lv_label_create(dbar);
    lv_label_set_text(title, idx < 0 ? "Neues Gerät" : "Gerät bearbeiten");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_center(title);

    lv_obj_t *save = lv_button_create(dbar);
    lv_obj_set_size(save, 170, 40);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(save, COL_OK, 0);
    lv_obj_add_event_cb(save, dev_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svl = lv_label_create(save);
    lv_label_set_text(svl, LV_SYMBOL_SAVE "  Speichern");
    lv_obj_set_style_text_color(svl, COL_ON_OK, 0); lv_obj_center(svl);

    /* Row 0: Name (free text) + the active switch on the right. */
    lv_obj_t *nl = lv_label_create(s_mb_dialog);
    lv_label_set_text(nl, "Name (Anzeige)"); lv_obj_set_style_text_color(nl, COL_SUB, 0);
    lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 16, 62);
    s_mb_name = mb_textarea(s_mb_dialog, "z. B. PV Garage", c.name, 560);
    lv_obj_align(s_mb_name, LV_ALIGN_TOP_LEFT, 16, 86);

    s_mb_en = lv_switch_create(s_mb_dialog);
    lv_obj_align(s_mb_en, LV_ALIGN_TOP_LEFT, 600, 88);
    if (c.enabled) lv_obj_add_state(s_mb_en, LV_STATE_CHECKED);
    lv_obj_t *el = lv_label_create(s_mb_dialog);
    lv_label_set_text(el, "aktiv"); lv_obj_set_style_text_color(el, COL_SUB, 0);
    lv_obj_align(el, LV_ALIGN_TOP_LEFT, 604, 62);

    /* Row 1: Hersteller + Geräte-Typ dropdowns */
    lv_obj_t *ml = lv_label_create(s_mb_dialog);
    lv_label_set_text(ml, "Hersteller"); lv_obj_set_style_text_color(ml, COL_SUB, 0);
    lv_obj_align(ml, LV_ALIGN_TOP_LEFT, 16, 130);
    s_mb_mfr_dd = lv_dropdown_create(s_mb_dialog);
    lv_dropdown_set_options(s_mb_mfr_dd, "Fronius\nDeye\nEltako");
    lv_dropdown_set_selected(s_mb_mfr_dd, c.mfr < MB_MFR_COUNT ? c.mfr : 0);
    lv_obj_set_width(s_mb_mfr_dd, 200);
    lv_obj_align(s_mb_mfr_dd, LV_ALIGN_TOP_LEFT, 16, 154);

    lv_obj_t *rl = lv_label_create(s_mb_dialog);
    lv_label_set_text(rl, "Geräte-Typ"); lv_obj_set_style_text_color(rl, COL_SUB, 0);
    lv_obj_align(rl, LV_ALIGN_TOP_LEFT, 234, 130);
    s_mb_role_dd = lv_dropdown_create(s_mb_dialog);
    lv_dropdown_set_options(s_mb_role_dd,
        "Zähler am Netzübergabepunkt\nZähler vor dem Wechselrichter\n"
        "Wechselrichter\nBatterie\nZähler vor dem Deye-WR");
    lv_dropdown_set_selected(s_mb_role_dd, c.role < MB_ROLE_COUNT ? c.role : 0);
    lv_obj_set_width(s_mb_role_dd, 340);
    lv_obj_align(s_mb_role_dd, LV_ALIGN_TOP_LEFT, 234, 154);

    char pb[8], ub[8], qb[8], tb[8];
    snprintf(pb, sizeof(pb), "%u", c.port);
    snprintf(ub, sizeof(ub), "%u", c.slave);
    snprintf(qb, sizeof(qb), "%u", c.poll_ms ? c.poll_ms : MB_DEFAULT_POLL_MS);
    snprintf(tb, sizeof(tb), "%u", c.timeout_ms ? c.timeout_ms : MB_DEFAULT_TIMEOUT_MS);

    /* Row 2: IP / Port / Slave-ID / Poll / Timeout */
    lv_obj_t *ipl = lv_label_create(s_mb_dialog);
    lv_label_set_text(ipl, "IP-Adresse"); lv_obj_set_style_text_color(ipl, COL_SUB, 0);
    lv_obj_align(ipl, LV_ALIGN_TOP_LEFT, 16, 206);
    s_mb_ip = mb_textarea(s_mb_dialog, "192.168.1.50", c.ip, 250);
    lv_obj_align(s_mb_ip, LV_ALIGN_TOP_LEFT, 16, 230);

    lv_obj_t *pl = lv_label_create(s_mb_dialog);
    lv_label_set_text(pl, "Port"); lv_obj_set_style_text_color(pl, COL_SUB, 0);
    lv_obj_align(pl, LV_ALIGN_TOP_LEFT, 286, 206);
    s_mb_port = mb_textarea(s_mb_dialog, "502", pb, 100);
    lv_obj_align(s_mb_port, LV_ALIGN_TOP_LEFT, 286, 230);

    lv_obj_t *ul = lv_label_create(s_mb_dialog);
    lv_label_set_text(ul, "Slave-ID"); lv_obj_set_style_text_color(ul, COL_SUB, 0);
    lv_obj_align(ul, LV_ALIGN_TOP_LEFT, 398, 206);
    s_mb_unit = mb_textarea(s_mb_dialog, "1", ub, 90);
    lv_obj_align(s_mb_unit, LV_ALIGN_TOP_LEFT, 398, 230);

    lv_obj_t *ql = lv_label_create(s_mb_dialog);
    lv_label_set_text(ql, "Poll (ms)"); lv_obj_set_style_text_color(ql, COL_SUB, 0);
    lv_obj_align(ql, LV_ALIGN_TOP_LEFT, 510, 206);
    s_mb_poll = mb_textarea(s_mb_dialog, "2000", qb, 120);
    lv_obj_align(s_mb_poll, LV_ALIGN_TOP_LEFT, 510, 230);

    lv_obj_t *tl2 = lv_label_create(s_mb_dialog);
    lv_label_set_text(tl2, "Timeout (ms)"); lv_obj_set_style_text_color(tl2, COL_SUB, 0);
    lv_obj_align(tl2, LV_ALIGN_TOP_LEFT, 642, 206);
    s_mb_tmo = mb_textarea(s_mb_dialog, "500", tb, 130);
    lv_obj_align(s_mb_tmo, LV_ALIGN_TOP_LEFT, 642, 230);

    /* Delete (only when editing): placed below the IP row (y=280, clear of y≈274). */
    if (idx >= 0) {
        lv_obj_t *del = lv_button_create(s_mb_dialog);
        lv_obj_set_size(del, 170, 44);
        lv_obj_align(del, LV_ALIGN_TOP_LEFT, 16, 280);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xe0564f), 0);
        lv_obj_add_event_cb(del, dev_del_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text(dl, LV_SYMBOL_TRASH "  Löschen"); lv_obj_center(dl);
    }

    /* Keyboard: always visible, immediately connected to the name field so the
     * dialog opens ready to type (no dead area below content). Numeric fields
     * switch it to NUMBER mode in mb_ta_focus_cb; ✓/✕ hide it. */
    s_mb_kbd = lv_keyboard_create(s_mb_dialog);
    lv_obj_set_size(s_mb_kbd, LV_PCT(100), 160);
    lv_obj_align(s_mb_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_mb_kbd, mb_kbd_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_mb_kbd, mb_kbd_cb, LV_EVENT_CANCEL, NULL);
    lv_keyboard_set_map(s_mb_kbd, LV_KEYBOARD_MODE_TEXT_LOWER,
                        s_kb_de_lower, s_kb_ctrl_de_lower);
    lv_keyboard_set_map(s_mb_kbd, LV_KEYBOARD_MODE_TEXT_UPPER,
                        s_kb_de_upper, s_kb_ctrl_de_upper);
    /* Default: German text keyboard on the name field. */
    lv_keyboard_set_mode(s_mb_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_mb_kbd, s_mb_name);
    s_active_ta = s_mb_name;
}

static void dev_item_cb(lv_event_t *e)
{
    open_dev_dialog((int)(intptr_t)lv_event_get_user_data(e));
}

static void dev_add_cb(lv_event_t *e) { (void)e; open_dev_dialog(-1); }

static void modbus_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 14, 0);

    s_mb_status = wrap_label(parent, LV_PCT(100), 0, 0, "Modbus: -");

    /* The "+ Gerät" button lives in the top menu bar (see ui_settings_create);
     * it is only shown while this tab is active. */

    s_mb_list = lv_list_create(parent);
    lv_obj_set_size(s_mb_list, LV_PCT(100), 336);
    lv_obj_align(s_mb_list, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_style_bg_color(s_mb_list, COL_PANEL, 0);
    lv_obj_set_style_border_color(s_mb_list, COL_PANEL2, 0);
    lv_obj_set_style_border_width(s_mb_list, 1, 0);

    mb_rebuild_list();
    mb_refresh();
}

/* ------------------- Modbus-RTU tab ---------------------------------- */

static const uint32_t RTU_BAUDS[] = { 4800, 9600, 19200, 38400 };
#define RTU_BAUD_OPTS "4800\n9600\n19200\n38400"

/* Listen port for the TCP<->RTU bridge. A fixed list rather than a text field:
 * this tab has no keyboard, and 502 is the standard Modbus-TCP port -- the rest
 * are the usual fallbacks for when something else already owns 502. */
static const uint32_t GW_PORTS[] = { 502, 503, 1502, 5020 };
#define GW_PORT_OPTS "502\n503\n1502\n5020"

static void mb_rtu_refresh(void)
{
    if (!s_rtu_status) return;
    mb_rtu_status_t st;
    modbus_rtu_get_status(&st);
    char buf[220];
    int o = 0;
    bool warn = false;
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        mb_rtu_bus_status_t *b = &st.bus[i];
        const char *busn = (i == 0) ? "Bus A" : "Bus B";
        if (!b->running) {
            o += snprintf(buf + o, sizeof(buf) - o, "%s%s: aus", i ? "\n" : "", busn);
        } else if (b->role == MB_RTU_SLAVE) {
            o += snprintf(buf + o, sizeof(buf) - o, "%s%s: Slave   Polls %u   Netz %.0f W",
                          i ? "\n" : "", busn, (unsigned)b->polls, b->a);
        } else {
            warn |= !b->online;
            o += snprintf(buf + o, sizeof(buf) - o,
                          "%s%s: Master %s   Polls %u  Fehler %u   Akku %.0f W / %.0f %%",
                          i ? "\n" : "", busn, b->online ? "online" : "offline",
                          (unsigned)b->polls, (unsigned)b->errs, b->a, b->b);
        }
    }
    lv_label_set_text(s_rtu_status, buf);
    lv_obj_set_style_text_color(s_rtu_status, warn ? COL_WARN : COL_SUB, 0);

    if (s_rtu_selftest_lbl) {
        mb_rtu_selftest_result_t r = modbus_rtu_selftest_result();
        char rbuf[100];
        lv_color_t col = COL_SUB;
        switch (r.state) {
        case MB_RTU_SELFTEST_IDLE:
            snprintf(rbuf, sizeof(rbuf), "—");
            break;
        case MB_RTU_SELFTEST_PENDING:
            snprintf(rbuf, sizeof(rbuf), "L\xc3\xa4uft...");
            col = COL_WARN;
            break;
        case MB_RTU_SELFTEST_PASS:
            snprintf(rbuf, sizeof(rbuf), LV_SYMBOL_OK "  PASS  (%ld ms)", (long)r.latency_ms);
            col = COL_OK;
            break;
        case MB_RTU_SELFTEST_FAIL:
        default:
            snprintf(rbuf, sizeof(rbuf), LV_SYMBOL_CLOSE "  FAIL: %s", r.error);
            col = COL_WARN;
            break;
        }
        lv_label_set_text(s_rtu_selftest_lbl, rbuf);
        lv_obj_set_style_text_color(s_rtu_selftest_lbl, col, 0);
    }

    if (s_gw_status) {
        modbus_gw_status_t g;
        modbus_gw_get_status(&g);
        mb_rtu_cfg_t c; modbus_rtu_get_cfg(&c);

        char gbuf[190];
        lv_color_t gcol = COL_SUB;
        if (!c.gw_enabled) {
            snprintf(gbuf, sizeof(gbuf), "Bridge: aus");
        } else if (!g.running) {
            snprintf(gbuf, sizeof(gbuf), "Bridge: Port %u nicht belegbar", (unsigned)c.gw_port);
            gcol = COL_WARN;
        } else {
            /* Which buses actually carry bridge traffic -- a bus that is off or
             * set to Slave is silently NOT bridged, and that is exactly the
             * case worth spelling out here. */
            char busy[40]; int bo = 0;
            for (int i = 0; i < MB_RTU_BUSES; i++) {
                if (!modbus_rtu_bus_can_gateway(i)) continue;
                bo += snprintf(busy + bo, sizeof(busy) - bo, "%s%s",
                               bo ? "+" : "", i == 0 ? "A" : "B");
            }
            if (!bo) {
                snprintf(busy, sizeof(busy), "kein Bus (Master noetig)");
                gcol = COL_WARN;
            }
            snprintf(gbuf, sizeof(gbuf),
                     "Bridge: Port %u   Bus %s   Clients %u/%u   OK %u  Fehler %u  Abgew. %u",
                     (unsigned)c.gw_port, busy, (unsigned)g.clients,
                     (unsigned)c.gw_max_clients, (unsigned)g.req_ok,
                     (unsigned)g.req_err, (unsigned)g.conn_rej);
            /* "Abgew." counts connections that never became a client. It is the
             * only visible symptom of a dry lwIP socket pool -- a client that
             * cannot connect never sends a request, so OK/Fehler both stay put
             * while the LAN sees a refused port. Non-zero here means look at
             * the socket budget, not at RS485. */
            if (g.conn_rej) gcol = COL_WARN;
        }
        lv_label_set_text(s_gw_status, gbuf);
        lv_obj_set_style_text_color(s_gw_status, gcol, 0);
    }
}

static void rtu_selftest_cb(lv_event_t *e)
{
    (void)e;
    modbus_rtu_selftest_start();
    mb_rtu_refresh();
}

static void rtu_save_cb(lv_event_t *e)
{
    (void)e;
    /* Start from the STORED config, not from zero: this tab owns only the bus
     * rows and the three bridge widgets below, so every other field (and every
     * field appended in the future) must survive a save untouched. */
    mb_rtu_cfg_t c;
    modbus_rtu_get_cfg(&c);
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        c.bus[i].enabled  = lv_obj_has_state(s_rtu_sw[i], LV_STATE_CHECKED) ? 1 : 0;
        c.bus[i].role     = (uint8_t)lv_dropdown_get_selected(s_rtu_role[i]); /* 0=Master,1=Slave */
        c.bus[i].slave_id = (uint8_t)(lv_dropdown_get_selected(s_rtu_id[i]) + 1);
        c.bus[i].baud     = RTU_BAUDS[lv_dropdown_get_selected(s_rtu_baud[i])];
    }
    /* TCP<->RTU bridge. Built after the bus rows, so the widgets may not exist
     * yet while a bus dropdown fires during tab construction -- the stored
     * values then simply stay as loaded above. */
    if (s_gw_sw && s_gw_port) {
        c.gw_enabled = lv_obj_has_state(s_gw_sw, LV_STATE_CHECKED) ? 1 : 0;
        c.gw_port    = (uint16_t)GW_PORTS[lv_dropdown_get_selected(s_gw_port)];
        c.gw_bus_mask = 0;
        for (int i = 0; i < MB_RTU_BUSES; i++)
            if (s_gw_bus[i] && lv_obj_has_state(s_gw_bus[i], LV_STATE_CHECKED))
                c.gw_bus_mask |= (uint8_t)(1u << i);
    }
    modbus_rtu_set_cfg(&c);
    mb_rtu_refresh();
}

/* One bus row: title + on/off switch + Rolle + Slave-ID + Baud dropdowns. */
static void rtu_row(lv_obj_t *parent, int idx, int y, const char *title,
                    const mb_rtu_bus_cfg_t *cfg)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, F_MD, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, y);

    s_rtu_sw[idx] = lv_switch_create(parent);
    /* vertically centred on the dropdown boxes (which sit at y+48, ~40 tall) */
    lv_obj_align(s_rtu_sw[idx], LV_ALIGN_TOP_LEFT, 0, y + 56);
    if (cfg->enabled) lv_obj_add_state(s_rtu_sw[idx], LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_rtu_sw[idx], rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rl = lv_label_create(parent);
    lv_label_set_text(rl, "Rolle"); lv_obj_set_style_text_color(rl, COL_SUB, 0);
    lv_obj_align(rl, LV_ALIGN_TOP_LEFT, 100, y + 28);
    s_rtu_role[idx] = lv_dropdown_create(parent);
    lv_dropdown_set_options(s_rtu_role[idx], "Master\nSlave");
    lv_dropdown_set_selected(s_rtu_role[idx], cfg->role == MB_RTU_SLAVE ? 1 : 0);
    lv_obj_set_width(s_rtu_role[idx], 180);
    lv_obj_align(s_rtu_role[idx], LV_ALIGN_TOP_LEFT, 100, y + 48);
    lv_obj_add_event_cb(s_rtu_role[idx], rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *il = lv_label_create(parent);
    lv_label_set_text(il, "Slave-ID"); lv_obj_set_style_text_color(il, COL_SUB, 0);
    lv_obj_align(il, LV_ALIGN_TOP_LEFT, 296, y + 28);
    s_rtu_id[idx] = lv_dropdown_create(parent);
    {
        char ids[80]; int o = 0;
        for (int i = 1; i <= 16; i++) o += snprintf(ids + o, sizeof(ids) - o, i > 1 ? "\n%d" : "%d", i);
        lv_dropdown_set_options(s_rtu_id[idx], ids);
    }
    lv_dropdown_set_selected(s_rtu_id[idx], (cfg->slave_id >= 1 && cfg->slave_id <= 16) ? cfg->slave_id - 1 : 0);
    lv_obj_set_width(s_rtu_id[idx], 80);
    lv_obj_align(s_rtu_id[idx], LV_ALIGN_TOP_LEFT, 296, y + 48);
    lv_obj_add_event_cb(s_rtu_id[idx], rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bl = lv_label_create(parent);
    lv_label_set_text(bl, "Baud"); lv_obj_set_style_text_color(bl, COL_SUB, 0);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 392, y + 28);
    s_rtu_baud[idx] = lv_dropdown_create(parent);
    lv_dropdown_set_options(s_rtu_baud[idx], RTU_BAUD_OPTS);
    lv_dropdown_set_selected(s_rtu_baud[idx],
                             opt_idx(RTU_BAUDS, ARRAY_LEN(RTU_BAUDS), cfg->baud, 1));
    lv_obj_set_width(s_rtu_baud[idx], 120);
    lv_obj_align(s_rtu_baud[idx], LV_ALIGN_TOP_LEFT, 392, y + 48);
    lv_obj_add_event_cb(s_rtu_baud[idx], rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* TCP<->RTU bridge: on/off, listen port, and which buses are reachable.
 * Everything else (timeout, client limit) keeps its default -- see
 * clamp_cfg() in modbus_rtu.c. */
static void gw_section_build(lv_obj_t *parent, const mb_rtu_cfg_t *c, int y)
{
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_set_size(sep, 700, 1);
    lv_obj_set_style_bg_color(sep, COL_PANEL2, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, "TCP-Bridge   (Modbus-TCP \xe2\x86\x92 RTU)");
    lv_obj_set_style_text_font(t, F_MD, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, y + 14);

    s_gw_sw = lv_switch_create(parent);
    lv_obj_align(s_gw_sw, LV_ALIGN_TOP_LEFT, 0, y + 62);
    if (c->gw_enabled) lv_obj_add_state(s_gw_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_gw_sw, rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *pl = lv_label_create(parent);
    lv_label_set_text(pl, "Port"); lv_obj_set_style_text_color(pl, COL_SUB, 0);
    lv_obj_align(pl, LV_ALIGN_TOP_LEFT, 100, y + 42);
    s_gw_port = lv_dropdown_create(parent);
    lv_dropdown_set_options(s_gw_port, GW_PORT_OPTS);
    lv_dropdown_set_selected(s_gw_port,
                             opt_idx(GW_PORTS, ARRAY_LEN(GW_PORTS), c->gw_port, 0));
    lv_obj_set_width(s_gw_port, 120);
    lv_obj_align(s_gw_port, LV_ALIGN_TOP_LEFT, 100, y + 62);
    lv_obj_add_event_cb(s_gw_port, rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bl = lv_label_create(parent);
    lv_label_set_text(bl, "Erreichbare Busse");
    lv_obj_set_style_text_color(bl, COL_SUB, 0);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 240, y + 42);
    for (int i = 0; i < MB_RTU_BUSES; i++) {
        s_gw_bus[i] = make_checkbox(parent, i == 0 ? "Bus A" : "Bus B",
                                    240 + i * 110, y + 66,
                                    c->gw_bus_mask & (1u << i));
        lv_obj_add_event_cb(s_gw_bus[i], rtu_save_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* Only a MASTER bus can be bridged -- on a Slave bus the Deye is the master
     * and our request would collide with its polling. Say so up front. */
    wrap_label(parent, 700, 0, y + 104,
        "Nur Busse in der Rolle Master werden gebrueckt. Unit-ID = Slave-ID des "
        "Busses; ist nur ein Bus gebrueckt, wird jede Unit-ID an ihn "
        "durchgereicht.");

    s_gw_status = wrap_label(parent, 700, 0, y + 152, "Bridge: -");
}

static void modbus_rtu_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    /* Scrollable (unlike the other tabs): the bridge section pushes the content
     * past the panel height. */
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_style_pad_all(parent, 14, 0);

    mb_rtu_cfg_t c; modbus_rtu_get_cfg(&c);

    s_rtu_status = wrap_label(parent, LV_PCT(100), 0, 0, "RTU: -");

    rtu_row(parent, 0, 50,  "Bus A  (UART1, GPIO52/51)", &c.bus[0]);
    rtu_row(parent, 1, 152, "Bus B  (UART2, GPIO50/49)", &c.bus[1]);

    /* Selftest: connect bus A ↔ bus B and press the button. */
    lv_obj_t *stbtn = lv_button_create(parent);
    lv_obj_set_size(stbtn, 180, 40);
    lv_obj_align(stbtn, LV_ALIGN_TOP_LEFT, 0, 262);
    lv_obj_set_style_bg_color(stbtn, COL_ACCENT, 0);
    lv_obj_add_event_cb(stbtn, rtu_selftest_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stl = lv_label_create(stbtn);
    lv_label_set_text(stl, LV_SYMBOL_REFRESH "  Selbsttest");
    lv_obj_set_style_text_color(stl, COL_TEXT, 0);
    lv_obj_center(stl);

    /* em-dash = "no self-test run yet" */
    s_rtu_selftest_lbl = wrap_label(parent, 560, 196, 272, "\xe2\x80\x94");

    gw_section_build(parent, &c, 320);

    /* Save button is in the top menu bar (header_save_cb). */

    mb_rtu_refresh();
}

/* ------------------- MQTT tab ---------------------------------------- */

static void mqtt_ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    s_active_ta = ta;
    bool num = (bool)(intptr_t)lv_obj_get_user_data(ta);
    lv_keyboard_set_mode(s_mqtt_kbd, num ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_mqtt_kbd, ta);
    lv_obj_remove_flag(s_mqtt_kbd, LV_OBJ_FLAG_HIDDEN);
}

static void mqtt_kbd_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_mqtt_kbd, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mqtt_ta(lv_obj_t *parent, const char *ph, const char *val,
                         int x, int y, int w, bool pass, bool num)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, ph);
    if (val && val[0]) lv_textarea_set_text(ta, val);
    if (pass) lv_textarea_set_password_mode(ta, true);
    lv_obj_set_width(ta, w);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_user_data(ta, (void *)(intptr_t)num);
    lv_obj_add_event_cb(ta, mqtt_ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void mqtt_refresh(void)
{
    if (!s_mqtt_status) return;
    mqtt_fwd_status_t st;
    mqtt_fwd_get_status(&st);
    char buf[96];
    snprintf(buf, sizeof(buf), "MQTT: %s   veröffentlicht %u",
             st.enabled ? (st.connected ? "verbunden" : "verbinde…") : "aus",
             (unsigned)st.published);
    lv_label_set_text(s_mqtt_status, buf);
    lv_obj_set_style_text_color(s_mqtt_status,
        st.connected ? COL_OK : (st.enabled ? COL_WARN : COL_SUB), 0);
}

static void mqtt_save_cb(lv_event_t *e)
{
    (void)e;
    mqtt_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = lv_obj_has_state(s_mqtt_en, LV_STATE_CHECKED) ? 1 : 0;
    strncpy(c.host, lv_textarea_get_text(s_mqtt_host), sizeof(c.host) - 1);
    int port = atoi(lv_textarea_get_text(s_mqtt_port));
    c.port = (port > 0 && port <= 65535) ? (uint16_t)port : 1883;
    strncpy(c.user, lv_textarea_get_text(s_mqtt_user), sizeof(c.user) - 1);
    strncpy(c.pass, lv_textarea_get_text(s_mqtt_pass), sizeof(c.pass) - 1);
    strncpy(c.base, lv_textarea_get_text(s_mqtt_base), sizeof(c.base) - 1);
    if (c.base[0] == '\0') strncpy(c.base, "deye-display", sizeof(c.base) - 1);
    c.retain    = lv_obj_has_state(s_mqtt_retain, LV_STATE_CHECKED) ? 1 : 0;
    c.discovery = lv_obj_has_state(s_mqtt_disc,   LV_STATE_CHECKED) ? 1 : 0;
    c.lastwill  = lv_obj_has_state(s_mqtt_lwt,    LV_STATE_CHECKED) ? 1 : 0;
    mqtt_fwd_set_cfg(&c);
    mqtt_refresh();
}

static void mqtt_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 14, 0);

    mqtt_cfg_t c; mqtt_fwd_get_cfg(&c);
    char portb[8]; snprintf(portb, sizeof(portb), "%u", c.port ? c.port : 1883);

    s_mqtt_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_mqtt_status, F_SM, 0);
    lv_obj_set_style_text_color(s_mqtt_status, COL_SUB, 0);
    lv_obj_align(s_mqtt_status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_mqtt_status, "MQTT: -");

    s_mqtt_en = lv_switch_create(parent);
    lv_obj_align(s_mqtt_en, LV_ALIGN_TOP_LEFT, 0, 22);
    if (c.enabled) lv_obj_add_state(s_mqtt_en, LV_STATE_CHECKED);
    lv_obj_t *el = lv_label_create(parent);
    lv_label_set_text(el, "MQTT aktiv"); lv_obj_set_style_text_color(el, COL_SUB, 0);
    lv_obj_align(el, LV_ALIGN_TOP_LEFT, 62, 28);

    /* Save button is in the top menu bar (header_save_cb); left column = broker
     * text fields, right column = Port + checkboxes. */
    s_mqtt_host = mqtt_ta(parent, "Broker-Host / IP", c.host, 0,   58, 280, false, false);
    s_mqtt_port = mqtt_ta(parent, "1883",             portb,  300, 58, 120, false, true);
    s_mqtt_user = mqtt_ta(parent, "Benutzer",         c.user, 0,   98, 280, false, false);
    s_mqtt_pass = mqtt_ta(parent, "Passwort",         c.pass, 0,  138, 280, true,  false);
    s_mqtt_base = mqtt_ta(parent, "Basis-Topic",      c.base, 0,  178, 280, false, false);

    /* Three checkboxes evenly spaced (40 px), aligned with the left column. */
    s_mqtt_retain = make_checkbox(parent, "Retain",       300, 104, c.retain);
    s_mqtt_disc   = make_checkbox(parent, "HA Discovery", 300, 144, c.discovery);
    s_mqtt_lwt    = make_checkbox(parent, "Last Will",    300, 184, c.lastwill);

    s_mqtt_kbd = lv_keyboard_create(parent);
    lv_obj_set_size(s_mqtt_kbd, LV_PCT(100), 174);
    lv_obj_align(s_mqtt_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_mqtt_kbd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_mqtt_kbd, mqtt_kbd_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_mqtt_kbd, mqtt_kbd_cb, LV_EVENT_CANCEL, NULL);

    mqtt_refresh();
}


/* ------------------- Zeit / NTP tab --------------------------------- */

static void ntp_ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    s_active_ta = ta;
    lv_keyboard_set_mode(s_ntp_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_ntp_kbd, ta);
    lv_obj_remove_flag(s_ntp_kbd, LV_OBJ_FLAG_HIDDEN);
}

static void ntp_kbd_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_ntp_kbd, LV_OBJ_FLAG_HIDDEN);
}

static void ntp_refresh(void)
{
    if (!s_ntp_status) return;
    char buf[96];
    if (ntp_is_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(buf, sizeof(buf),
                 "%02d.%02d.%04d  %02d:%02d:%02d   " LV_SYMBOL_OK " synchronisiert",
                 tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_obj_set_style_text_color(s_ntp_status, COL_OK, 0);
    } else {
        snprintf(buf, sizeof(buf), "nicht synchronisiert");
        lv_obj_set_style_text_color(s_ntp_status, COL_SUB, 0);
    }
    lv_label_set_text(s_ntp_status, buf);
}

static void ntp_save_cb(lv_event_t *e)
{
    (void)e;
    ntp_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = lv_obj_has_state(s_ntp_en, LV_STATE_CHECKED) ? 1 : 0;
    c.tz_idx  = (uint8_t)lv_dropdown_get_selected(s_ntp_tz);
    strncpy(c.server, lv_textarea_get_text(s_ntp_server), sizeof(c.server) - 1);
    if (c.server[0] == '\0') strncpy(c.server, "pool.ntp.org", sizeof(c.server) - 1);
    ntp_set_cfg(&c);
    ntp_refresh();
}

static void ntp_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 14, 0);

    ntp_cfg_t c;
    ntp_get_cfg(&c);

    s_ntp_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_ntp_status, F_SM, 0);
    lv_obj_set_style_text_color(s_ntp_status, COL_SUB, 0);
    lv_obj_align(s_ntp_status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_ntp_status, "Zeit: -");

    s_ntp_en = lv_switch_create(parent);
    lv_obj_align(s_ntp_en, LV_ALIGN_TOP_LEFT, 0, 26);
    if (c.enabled) lv_obj_add_state(s_ntp_en, LV_STATE_CHECKED);
    lv_obj_t *el = lv_label_create(parent);
    lv_label_set_text(el, "NTP aktiv");
    lv_obj_set_style_text_color(el, COL_SUB, 0);
    lv_obj_align(el, LV_ALIGN_TOP_LEFT, 62, 32);

    /* NTP server (own keyboard). */
    lv_obj_t *sl = lv_label_create(parent);
    lv_label_set_text(sl, "NTP-Server");
    lv_obj_set_style_text_color(sl, COL_SUB, 0);
    lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 0, 76);
    s_ntp_server = lv_textarea_create(parent);
    lv_textarea_set_one_line(s_ntp_server, true);
    lv_textarea_set_placeholder_text(s_ntp_server, "pool.ntp.org");
    if (c.server[0]) lv_textarea_set_text(s_ntp_server, c.server);
    lv_obj_set_width(s_ntp_server, 320);
    lv_obj_align(s_ntp_server, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_set_style_bg_color(s_ntp_server, COL_PANEL, 0);
    lv_obj_set_style_text_color(s_ntp_server, COL_TEXT, 0);
    lv_obj_add_event_cb(s_ntp_server, ntp_ta_focus_cb, LV_EVENT_CLICKED, NULL);

    /* Timezone dropdown. */
    lv_obj_t *tl = lv_label_create(parent);
    lv_label_set_text(tl, "Zeitzone");
    lv_obj_set_style_text_color(tl, COL_SUB, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 150);
    s_ntp_tz = lv_dropdown_create(parent);
    char opts[384];
    opts[0] = '\0';
    for (int i = 0; i < NTP_TZ_COUNT; i++) {
        strncat(opts, ntp_tz_name(i), sizeof(opts) - strlen(opts) - 1);
        if (i < NTP_TZ_COUNT - 1) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
    }
    lv_dropdown_set_options(s_ntp_tz, opts);
    lv_dropdown_set_selected(s_ntp_tz, c.tz_idx);
    lv_obj_set_width(s_ntp_tz, 320);
    lv_obj_align(s_ntp_tz, LV_ALIGN_TOP_LEFT, 0, 174);

    s_ntp_kbd = lv_keyboard_create(parent);
    lv_obj_set_size(s_ntp_kbd, LV_PCT(100), 174);
    lv_obj_align(s_ntp_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_ntp_kbd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ntp_kbd, ntp_kbd_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_ntp_kbd, ntp_kbd_cb, LV_EVENT_CANCEL, NULL);

    ntp_refresh();
}


/* ------------------- VPN / WireGuard tab ---------------------------- */

static void vpn_ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    s_active_ta = ta;
    bool num = (bool)(intptr_t)lv_obj_get_user_data(ta);
    lv_keyboard_set_mode(s_vpn_kbd, num ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_vpn_kbd, ta);   /* also scrolls it above the kbd */
    lv_obj_remove_flag(s_vpn_kbd, LV_OBJ_FLAG_HIDDEN);
}

static void vpn_kbd_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_vpn_kbd, LV_OBJ_FLAG_HIDDEN);
}

/* Labelled one-line textarea in the scrollable column. num -> numeric keyboard. */
static lv_obj_t *vpn_field(lv_obj_t *parent, const char *label, const char *val, bool num)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, F_SM, 0);
    lv_obj_set_style_text_color(l, COL_SUB, 0);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    if (val && val[0]) lv_textarea_set_text(ta, val);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_bg_color(ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_user_data(ta, (void *)(intptr_t)num);
    lv_obj_add_event_cb(ta, vpn_ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void vpn_refresh(void)
{
    if (!s_vpn_status) return;
    wg_status_t st;
    wg_get_status(&st);
    char buf[80];
    if (!st.enabled) {
        snprintf(buf, sizeof(buf), "VPN: aus");
        lv_obj_set_style_text_color(s_vpn_status, COL_SUB, 0);
    } else if (st.up) {
        snprintf(buf, sizeof(buf), "VPN: verbunden  (%s)", st.address);
        lv_obj_set_style_text_color(s_vpn_status, COL_OK, 0);
    } else {
        snprintf(buf, sizeof(buf), "VPN: Handshake…");
        lv_obj_set_style_text_color(s_vpn_status, COL_WARN, 0);
    }
    lv_label_set_text(s_vpn_status, buf);
}

static void vpn_save_cb(lv_event_t *e)
{
    (void)e;
    wg_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = lv_obj_has_state(s_vpn_en, LV_STATE_CHECKED) ? 1 : 0;
    strncpy(c.private_key,   lv_textarea_get_text(s_vpn_privkey),  sizeof(c.private_key)   - 1);
    strncpy(c.public_key,    lv_textarea_get_text(s_vpn_pubkey),   sizeof(c.public_key)    - 1);
    strncpy(c.preshared_key, lv_textarea_get_text(s_vpn_psk),      sizeof(c.preshared_key) - 1);
    strncpy(c.endpoint,      lv_textarea_get_text(s_vpn_endpoint), sizeof(c.endpoint)      - 1);
    strncpy(c.address,       lv_textarea_get_text(s_vpn_addr),     sizeof(c.address)       - 1);
    strncpy(c.netmask,       lv_textarea_get_text(s_vpn_mask),     sizeof(c.netmask)       - 1);
    if (c.netmask[0] == '\0') strncpy(c.netmask, "255.255.255.0", sizeof(c.netmask) - 1);
    int port = atoi(lv_textarea_get_text(s_vpn_port));
    c.port = (port > 0 && port <= 65535) ? (uint16_t)port : 51820;
    int ka = atoi(lv_textarea_get_text(s_vpn_keep));
    c.keepalive = (ka >= 0 && ka <= 65535) ? (uint16_t)ka : 25;
    wg_set_cfg(&c);
    vpn_refresh();
}

static void vpn_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 14, 0);
    /* Scrollable: more fields than fit, and the keyboard overlays the bottom. */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(parent, 6, 0);

    wg_cfg_t c;
    wg_get_cfg(&c);
    char portb[8]; snprintf(portb, sizeof(portb), "%u", c.port ? c.port : 51820);
    char keepb[8]; snprintf(keepb, sizeof(keepb), "%u", c.keepalive);

    s_vpn_status = lv_label_create(parent);
    lv_obj_set_style_text_font(s_vpn_status, F_SM, 0);
    lv_obj_set_style_text_color(s_vpn_status, COL_SUB, 0);
    lv_label_set_text(s_vpn_status, "VPN: -");

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_vpn_en = lv_switch_create(row);
    if (c.enabled) lv_obj_add_state(s_vpn_en, LV_STATE_CHECKED);
    lv_obj_t *el = lv_label_create(row);
    lv_label_set_text(el, "WireGuard aktiv");
    lv_obj_set_style_text_color(el, COL_SUB, 0);

    s_vpn_privkey  = vpn_field(parent, "Privater Schluessel (dieses Geraet)", c.private_key,   false);
    s_vpn_pubkey   = vpn_field(parent, "Public Key (Peer / Server)",          c.public_key,    false);
    s_vpn_psk      = vpn_field(parent, "Preshared Key (optional)",            c.preshared_key, false);
    s_vpn_endpoint = vpn_field(parent, "Endpoint (Host / IP)",                c.endpoint,      false);
    s_vpn_port     = vpn_field(parent, "Endpoint-Port",                       portb,           true);
    s_vpn_addr     = vpn_field(parent, "Tunnel-IP (dieses Geraet)",           c.address,       false);
    s_vpn_mask     = vpn_field(parent, "Netzmaske",                           c.netmask,       false);
    s_vpn_keep     = vpn_field(parent, "Keepalive (s, 0 = aus)",              keepb,           true);

    /* Keyboard overlays the whole settings screen bottom (not the scrolling
     * page), so it stays put while the focused field scrolls above it. */
    s_vpn_kbd = lv_keyboard_create(s_screen);
    lv_obj_set_size(s_vpn_kbd, LV_PCT(100), 174);
    lv_obj_align(s_vpn_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_vpn_kbd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_vpn_kbd, vpn_kbd_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_vpn_kbd, vpn_kbd_cb, LV_EVENT_CANCEL, NULL);

    vpn_refresh();
}


/* ------------------- tabs & shell ------------------------------------- */

/* The "Speichern" button lives in the top bar and saves whichever tab is
 * active (the per-tab save callbacks ignore the event arg). */
static void header_save_cb(lv_event_t *e)
{
    (void)e;
    switch (s_active_tab) {
    case TAB_DISPLAY: sleep_save_cb(NULL); break;
    case TAB_MBRTU:   rtu_save_cb(NULL);   break;
    case TAB_MQTT:    mqtt_save_cb(NULL);  break;
    case TAB_ZEIT:    ntp_save_cb(NULL);   break;
    case TAB_VPN:     vpn_save_cb(NULL);   break;
    default: break;
    }
}

static bool tab_has_save(tab_id_t id)
{
    return id == TAB_DISPLAY || id == TAB_MBRTU || id == TAB_MQTT ||
           id == TAB_ZEIT    || id == TAB_VPN;
}

static void tab_select(tab_id_t id)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        bool active = (i == (int)id);
        lv_obj_set_style_bg_color(s_tab_buttons[i],
                                  active ? COL_PANEL : COL_BG, 0);
        lv_obj_set_style_border_side(s_tab_buttons[i],
                                     active ? LV_BORDER_SIDE_LEFT
                                            : LV_BORDER_SIDE_NONE, 0);
        if (active) lv_obj_remove_flag(s_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_tab_pages[i],    LV_OBJ_FLAG_HIDDEN);
    }
    if (s_mb_add_btn) {
        if (id == TAB_MBTCP) lv_obj_remove_flag(s_mb_add_btn, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(s_mb_add_btn,    LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_scan_bar) {
        if (id == TAB_WIFI) lv_obj_remove_flag(s_wifi_scan_bar, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(s_wifi_scan_bar,    LV_OBJ_FLAG_HIDDEN);
    }
    if (s_save_bar) {
        if (tab_has_save(id)) lv_obj_remove_flag(s_save_bar, LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(s_save_bar,    LV_OBJ_FLAG_HIDDEN);
    }
    s_active_tab = id;
}

static lv_obj_t *tab_button_create(lv_obj_t *parent, const char *label,
                                   const char *icon, int y, tab_id_t id)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, TAB_W, TAB_H);
    lv_obj_set_pos(b, 0, y);
    lv_obj_set_style_bg_color(b, COL_BG, 0);
    lv_obj_set_style_border_color(b, COL_ACCENT, 0);
    lv_obj_set_style_border_width(b, 4, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, tab_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *ic = lv_label_create(b);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, F_LG, 0);
    lv_obj_set_style_text_color(ic, COL_TEXT, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *tx = lv_label_create(b);
    lv_label_set_text(tx, label);
    lv_obj_set_style_text_font(tx, F_MD, 0);
    lv_obj_set_style_text_color(tx, COL_TEXT, 0);
    lv_obj_align(tx, LV_ALIGN_LEFT_MID, 64, 0);
    return b;
}

/* ------------------- password dialog --------------------------------- */

static void open_pwd_dialog(const char *ssid)
{
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';

    /* Full-screen modal */
    s_pwd_dialog = lv_obj_create(s_screen);
    lv_obj_set_size(s_pwd_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_pwd_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_pwd_dialog, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_pwd_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pwd_dialog, 0, 0);
    lv_obj_clear_flag(s_pwd_dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar: Zurück (left) + title (center) + Verbinden (right), matching the
     * main settings screen and the device dialog. */
    lv_obj_t *pbar = lv_obj_create(s_pwd_dialog);
    lv_obj_set_size(pbar, LV_PCT(100), BAR_H);
    lv_obj_set_pos(pbar, 0, 0);
    lv_obj_set_style_bg_color(pbar, COL_PANEL, 0);
    lv_obj_set_style_border_width(pbar, 0, 0);
    lv_obj_set_style_radius(pbar, 0, 0);
    lv_obj_set_style_pad_all(pbar, 0, 0);
    lv_obj_clear_flag(pbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pback = lv_button_create(pbar);
    lv_obj_set_size(pback, 140, 44);
    lv_obj_align(pback, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(pback, COL_PANEL2, 0);
    lv_obj_add_event_cb(pback, pwd_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pbl = lv_label_create(pback);
    lv_label_set_text(pbl, LV_SYMBOL_LEFT "  Zurück"); lv_obj_center(pbl);

    lv_obj_t *hdr = lv_label_create(pbar);
    lv_label_set_text(hdr, "WLAN-Passwort");
    lv_obj_set_style_text_font(hdr, F_LG, 0);
    lv_obj_set_style_text_color(hdr, COL_TEXT, 0);
    lv_obj_center(hdr);

    lv_obj_t *connect = lv_button_create(pbar);
    lv_obj_set_size(connect, 150, 40);
    lv_obj_align(connect, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(connect, COL_OK, 0);
    lv_obj_add_event_cb(connect, pwd_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cnl = lv_label_create(connect);
    lv_label_set_text(cnl, LV_SYMBOL_OK "  Verbinden");
    lv_obj_set_style_text_color(cnl, COL_ON_OK, 0);
    lv_obj_center(cnl);

    /* SSID context, in the body below the bar. */
    lv_obj_t *ssl = lv_label_create(s_pwd_dialog);
    char buf[80];
    snprintf(buf, sizeof(buf), "Netzwerk:  %s", ssid);
    lv_label_set_text(ssl, buf);
    lv_obj_set_style_text_font(ssl, F_MD, 0);
    lv_obj_set_style_text_color(ssl, COL_SUB, 0);
    lv_obj_align(ssl, LV_ALIGN_TOP_LEFT, 16, 64);

    /* Textarea */
    s_pwd_ta = lv_textarea_create(s_pwd_dialog);
    lv_obj_set_size(s_pwd_ta, LV_PCT(96), 50);
    lv_obj_align(s_pwd_ta, LV_ALIGN_TOP_MID, 0, 96);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_textarea_set_password_mode(s_pwd_ta, false);
    lv_textarea_set_placeholder_text(s_pwd_ta, "WLAN-Passwort");
    lv_obj_set_style_bg_color(s_pwd_ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(s_pwd_ta, COL_TEXT, 0);

    /* Keyboard */
    s_pwd_kbd = lv_keyboard_create(s_pwd_dialog);
    lv_obj_set_size(s_pwd_kbd, LV_PCT(100), 320);
    lv_obj_align(s_pwd_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_pwd_kbd, s_pwd_ta);
    s_active_ta = s_pwd_ta;                  /* receive PC-keyboard keys */
    lv_obj_add_event_cb(s_pwd_kbd, kbd_ready_cb,
                        LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_pwd_kbd, kbd_ready_cb,
                        LV_EVENT_CANCEL, NULL);
}

static void close_pwd_dialog(void)
{
    if (s_pwd_dialog) {
        lv_obj_del(s_pwd_dialog);
        s_pwd_dialog = NULL;
        s_pwd_kbd    = NULL;
        s_pwd_ta     = NULL;
        s_active_ta  = NULL;
    }
}

/* ------------------- timer ------------------------------------------- */

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    wifi_refresh();
    mb_refresh();
    mb_rtu_refresh();
    mqtt_refresh();
    ntp_refresh();
    vpn_refresh();
}

/* ------------------- System tab (SLS export guard) ------------------- */

static const uint8_t  k_sls_amps[] = { 0, 16, 20, 25, 35, 50, 63 };
#define SLS_OPTS "Deaktiviert\n16 A\n20 A\n25 A\n35 A\n50 A\n63 A"

static void sls_update_status(uint8_t a)
{
    if (!s_sls_status_lbl) return;
    char buf[80];
    if (a == 0) {
        snprintf(buf, sizeof(buf), "Export-Begrenzung deaktiviert");
    } else {
        float max_kw = (float)a * 3.0f * 230.0f * 0.9f / 1000.0f;
        snprintf(buf, sizeof(buf), "Max. Export: %.1f kW  (%u A \xc3\x97 3 \xc3\x97 230 V \xc3\x97 90%%)",
                 max_kw, (unsigned)a);
    }
    lv_label_set_text(s_sls_status_lbl, buf);
}

static void sls_change_cb(lv_event_t *e)
{
    (void)e;
    uint32_t sel = lv_dropdown_get_selected(s_sls_dd);
    if (sel >= (uint32_t)ARRAY_LEN(k_sls_amps)) sel = 0;
    uint8_t a = k_sls_amps[sel];
    nvs_store_set_sls_a(a);
    sls_update_status(a);
    ESP_LOGI("ui_sys", "SLS saved: %u A", (unsigned)a);
}

static void system_tab_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 22, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(parent, 12, 0);

    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, "Netzanschluss (SLS-Schalter)");
    lv_obj_set_style_text_font(hdr, F_MD, 0);
    lv_obj_set_style_text_color(hdr, COL_TEXT, 0);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub,
        "Bei Zwangsentladung wird Register 143 (Entladeleistung)\n"
        "automatisch gedrosselt, sobald die Netzeinspeisung 90%\n"
        "des SLS-Nennstroms \xc3\xbc""berschreitet.");
    lv_obj_set_style_text_font(sub, F_SM, 0);
    lv_obj_set_style_text_color(sub, COL_SUB, 0);
    lv_obj_set_width(sub, 560);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    s_sls_dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(s_sls_dd, SLS_OPTS);
    lv_obj_set_width(s_sls_dd, 200);
    uint8_t cur_a = nvs_store_get_sls_a();
    for (int i = 0; i < ARRAY_LEN(k_sls_amps); i++) {
        if (k_sls_amps[i] == cur_a) { lv_dropdown_set_selected(s_sls_dd, i); break; }
    }
    lv_obj_add_event_cb(s_sls_dd, sls_change_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_sls_status_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(s_sls_status_lbl, F_SM, 0);
    lv_obj_set_style_text_color(s_sls_status_lbl, COL_OK, 0);
    sls_update_status(cur_a);
}

/* ------------------- public ------------------------------------------ */

void ui_settings_create(void)
{
    s_main_screen = lv_screen_active();
    s_screen      = lv_obj_create(NULL);       /* a free-standing screen */
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_text_color(s_screen, COL_TEXT, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_set_size(bar, 800, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 140, 44);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(back, COL_PANEL2, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Zurück");
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Einstellungen");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_center(title);

    /* "+ Gerät" in the menu bar (top-right). Only visible on the Mod-TCP tab. */
    s_mb_add_btn = lv_button_create(bar);
    lv_obj_set_size(s_mb_add_btn, 150, 40);
    lv_obj_align(s_mb_add_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_mb_add_btn, COL_ACCENT, 0);
    lv_obj_add_event_cb(s_mb_add_btn, dev_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(s_mb_add_btn);
    lv_label_set_text(al, LV_SYMBOL_PLUS "  Gerät"); lv_obj_center(al);
    lv_obj_add_flag(s_mb_add_btn, LV_OBJ_FLAG_HIDDEN);

    /* "Scan" in the menu bar (top-right). Only visible on the WLAN tab. */
    s_wifi_scan_bar = lv_button_create(bar);
    lv_obj_set_size(s_wifi_scan_bar, 150, 40);
    lv_obj_align(s_wifi_scan_bar, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_wifi_scan_bar, COL_ACCENT, 0);
    lv_obj_add_event_cb(s_wifi_scan_bar, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scl = lv_label_create(s_wifi_scan_bar);
    lv_label_set_text(scl, LV_SYMBOL_REFRESH "  Scan"); lv_obj_center(scl);
    lv_obj_add_flag(s_wifi_scan_bar, LV_OBJ_FLAG_HIDDEN);

    /* "Speichern" in the menu bar (top-right). Shown on Display/RTU/MQTT;
     * saves the active tab. Green with black label, diskette icon. */
    s_save_bar = lv_button_create(bar);
    lv_obj_set_size(s_save_bar, 170, 40);
    lv_obj_align(s_save_bar, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_save_bar, COL_OK, 0);
    lv_obj_add_event_cb(s_save_bar, header_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svb = lv_label_create(s_save_bar);
    lv_label_set_text(svb, LV_SYMBOL_SAVE "  Speichern");
    lv_obj_set_style_text_color(svb, COL_ON_OK, 0);
    lv_obj_center(svb);
    lv_obj_add_flag(s_save_bar, LV_OBJ_FLAG_HIDDEN);

    /* Left tab rail */
    lv_obj_t *rail = lv_obj_create(s_screen);
    lv_obj_set_size(rail, TAB_W, 480 - BAR_H);
    lv_obj_set_pos(rail, 0, BAR_H);
    lv_obj_set_style_bg_color(rail, COL_BG, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_set_style_pad_all(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    s_tab_buttons[TAB_WIFI]    = tab_button_create(rail, "WLAN",
                                LV_SYMBOL_WIFI,    0,             TAB_WIFI);
    s_tab_buttons[TAB_DISPLAY] = tab_button_create(rail, "Display",
                                LV_SYMBOL_IMAGE,   TAB_H,         TAB_DISPLAY);
    s_tab_buttons[TAB_MBTCP]   = tab_button_create(rail, "Mod TCP",
                                LV_SYMBOL_SHUFFLE, TAB_H * 2,     TAB_MBTCP);
    s_tab_buttons[TAB_MBRTU]   = tab_button_create(rail, "Mod RTU",
                                LV_SYMBOL_LIST,    TAB_H * 3,     TAB_MBRTU);
    s_tab_buttons[TAB_MQTT]    = tab_button_create(rail, "MQTT",
                                LV_SYMBOL_UPLOAD,  TAB_H * 4,     TAB_MQTT);
    s_tab_buttons[TAB_ZEIT]    = tab_button_create(rail, "Zeit",
                                LV_SYMBOL_BELL,    TAB_H * 5,     TAB_ZEIT);
    s_tab_buttons[TAB_VPN]     = tab_button_create(rail, "VPN",
                                LV_SYMBOL_GPS,     TAB_H * 6,     TAB_VPN);
    s_tab_buttons[TAB_SYSTEM]  = tab_button_create(rail, "System",
                                LV_SYMBOL_SETTINGS, TAB_H * 7,   TAB_SYSTEM);

    /* Right content area */
    for (int i = 0; i < TAB_COUNT; i++) {
        s_tab_pages[i] = lv_obj_create(s_screen);
        lv_obj_set_size(s_tab_pages[i], 800 - TAB_W, 480 - BAR_H);
        lv_obj_set_pos(s_tab_pages[i], TAB_W, BAR_H);
        lv_obj_set_style_bg_color(s_tab_pages[i], COL_BG, 0);
        lv_obj_set_style_border_width(s_tab_pages[i], 0, 0);
        lv_obj_set_style_radius(s_tab_pages[i], 0, 0);
        lv_obj_add_flag(s_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    wifi_tab_build(s_tab_pages[TAB_WIFI]);
    display_tab_build(s_tab_pages[TAB_DISPLAY]);
    modbus_tab_build(s_tab_pages[TAB_MBTCP]);
    modbus_rtu_tab_build(s_tab_pages[TAB_MBRTU]);
    mqtt_tab_build(s_tab_pages[TAB_MQTT]);
    ntp_tab_build(s_tab_pages[TAB_ZEIT]);
    vpn_tab_build(s_tab_pages[TAB_VPN]);
    system_tab_build(s_tab_pages[TAB_SYSTEM]);

    tab_select(TAB_WIFI);

    s_refresh_timer = lv_timer_create(refresh_timer_cb, 1500, NULL);
    lv_timer_pause(s_refresh_timer);
}

void ui_settings_open(void)
{
    if (!s_screen) return;     /* not built yet (early boot) -> ignore the tap */
    if (s_refresh_timer) lv_timer_resume(s_refresh_timer);
    wifi_refresh();
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

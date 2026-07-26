#include "ui_flow.h"
#include "ui_settings.h"
#include "build_info.h"
#include "wifi_mgr.h"
#include "nvs_store.h"
#include "display.h"
#include "modbus_tcp.h"
#include "modbus_rtu.h"
#include "deye_ctrl.h"
#include "ntp_client.h"
#include "fonts.h"
#include "lvgl.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

/* ---------- Palette (dark theme) ---------- */
#define COL_BG          lv_color_hex(0x000000)
#define COL_ARC_BG      lv_color_hex(0x1f2329)
#define COL_PANEL2      lv_color_hex(0x2a2e35)   /* slider track (matches settings) */
#define COL_TEXT        lv_color_hex(0xe5e7eb)
#define COL_SUB         lv_color_hex(0x9aa0a8)
#define COL_PV          lv_color_hex(0xf5c842)
#define COL_HOUSE       lv_color_hex(0x4ea8ff)
#define COL_BTN         lv_color_hex(0x2563eb)   /* darker blue for buttons */
#define COL_GRID        lv_color_hex(0xb0b6bf)
#define COL_BYD         lv_color_hex(0x4cd97b)
#define COL_DEYE        lv_color_hex(0x34c9b9)
#define COL_INVERTER    lv_color_hex(0xe5e7eb)
#define COL_OK          lv_color_hex(0x4cd97b)
#define COL_ERR         lv_color_hex(0xff5555)

/* ---------- Geometry ----------
 * House sits in the CENTER; the four sources/sinks sit in the corners and each
 * is joined to the house by an animated dotted flow line. */
#define SCREEN_W        800
#define SCREEN_H        480
#define HOUSE_CX        400
#define HOUSE_CY        240
#define HOUSE_DIA       162
#define NODE_DIA        128
#define CORNER_DX       232    /* corner node offset from center, X */
#define CORNER_DY       122    /* corner node offset from center, Y */
#define ARC_WIDTH       10

/* Animated flow lines (dotted): one per corner node -> house. */
#define FLOW_LINES      4      /* 0=PV, 1=Netz, 2=BYD, 3=Deye        */
#define FLOW_DOTS       7      /* moving dots per line (finer stream) */
#define FLOW_DOT_DIA    8      /* dot diameter at full size           */
#define FLOW_DOT_MIN    3      /* dot diameter where it fades in/out  */
#define FLOW_FADE       0.24f  /* fraction of travel spent fading     */
#define FLOW_SPEED      0.013f /* phase advance per frame (calm drift)*/

/* Default arc full-scale values (used to compute fill percentage) */
#define PV_MAX_KW       15.0f
#define HOUSE_MAX_KW    10.0f
#define GRID_MAX_KW     15.0f
#define BATT_MAX_KW     10.0f

/* Max forced charge/discharge power selectable in the Deye battery popup (W). */
#define DEYE_FORCE_MAX_W  22000

typedef struct {
    lv_obj_t   *arc;
    lv_obj_t   *icon;
    lv_obj_t   *val_lbl;
    lv_obj_t   *sub_lbl;     /* optional second line (e.g., SoC) */
    lv_color_t  accent;
} node_t;

static node_t s_pv, s_house, s_grid, s_byd, s_deye;
static lv_obj_t *s_build_lbl;
static lv_obj_t *s_clock_lbl;     /* top-center HH:MM         */
static lv_obj_t *s_date_lbl;      /* top-center weekday, date */
static lv_obj_t *s_wifi_lbl;
static lv_obj_t *s_wifi_popup;
static lv_obj_t *s_gen_popup;
static lv_obj_t *s_sp_lbl;        /* grid-setpoint value label */

/* Uptime / restart popup (tap the clock/date). */
static lv_obj_t  *s_sys_popup;
static lv_obj_t  *s_sys_uptime_lbl;
static lv_timer_t *s_sys_timer;
static lv_obj_t  *s_confirm_popup;

/* Deye battery-mode popup working state (only one popup open at a time). */
static lv_obj_t   *s_deye_popup;
static lv_obj_t   *s_deye_mode_btn[DEYE_MODE_COUNT];
static lv_obj_t   *s_deye_pwr_row;     /* power label+slider block (hidden in Normal) */
static lv_obj_t   *s_deye_pwr_lbl;
static lv_obj_t   *s_deye_pwr_slider;
static deye_mode_t s_deye_sel_mode;    /* mode currently selected in the popup */

/* ---------- Animated dotted flow lines ---------- */
typedef struct {
    int        ax, ay;       /* node-side endpoint (circle edge)  */
    int        bx, by;       /* house-side endpoint (circle edge) */
    int        dir;          /* +1: node->house, -1: house->node, 0: no flow */
    lv_color_t color;
    lv_obj_t  *line;         /* faint static guide                */
    lv_point_precise_t lpts[2];
    lv_obj_t  *dot[FLOW_DOTS];
} flow_t;

static flow_t  s_flow[FLOW_LINES];   /* 0=PV, 1=Netz, 2=BYD, 3=Deye */
static float   s_flow_phase;

static void settings_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

/* Grid-setpoint slider (zero-export trick): -1000..+1000 W, snapped to 50 W.
 * Live label on drag; persisted via modbus_rtu on release. */
static void grid_sp_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    int v = lv_slider_get_value(sl);
    v = ((v < 0 ? v - 25 : v + 25) / 50) * 50;        /* snap to 50 W */
    if (s_sp_lbl) {
        char b[24];
        snprintf(b, sizeof(b), "%+d W", v);
        lv_label_set_text(s_sp_lbl, b);
    }
    if (code == LV_EVENT_RELEASED) {
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        modbus_rtu_set_grid_setpoint(v);
    }
}

/* ---------------------------------------------------------------- */

/* Corner node centers (idx: 0=PV TL, 1=Netz TR, 2=BYD BL, 3=Deye BR). */
static void node_pos(int idx, int *x, int *y)
{
    *x = HOUSE_CX + ((idx & 1) ? CORNER_DX : -CORNER_DX);
    *y = HOUSE_CY + ((idx >= 2) ? CORNER_DY : -CORNER_DY);
}

static void node_build(node_t *n, lv_obj_t *parent, int cx, int cy, int dia,
                       const char *icon_str, lv_color_t accent)
{
    n->accent = accent;

    n->arc = lv_arc_create(parent);
    lv_obj_set_size(n->arc, dia, dia);
    lv_obj_set_pos(n->arc, cx - dia / 2, cy - dia / 2);
    lv_arc_set_rotation(n->arc, 270);
    lv_arc_set_bg_angles(n->arc, 0, 360);
    lv_arc_set_range(n->arc, 0, 1000);
    lv_arc_set_value(n->arc, 0);
    lv_obj_remove_style(n->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(n->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(n->arc, COL_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(n->arc, ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(n->arc, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(n->arc, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(n->arc, true, LV_PART_INDICATOR);

    n->icon = lv_label_create(n->arc);
    lv_label_set_text(n->icon, icon_str);
    lv_obj_set_style_text_font(n->icon, F_LG, 0);
    lv_obj_set_style_text_color(n->icon, accent, 0);
    lv_obj_align(n->icon, LV_ALIGN_CENTER, 0, -14);

    n->val_lbl = lv_label_create(n->arc);
    lv_label_set_text(n->val_lbl, "--");
    lv_obj_set_style_text_font(n->val_lbl, F_MD, 0);
    lv_obj_set_style_text_color(n->val_lbl, COL_TEXT, 0);
    lv_obj_align(n->val_lbl, LV_ALIGN_CENTER, 0, 14);

    n->sub_lbl = lv_label_create(n->arc);
    lv_label_set_text(n->sub_lbl, "");
    lv_obj_set_style_text_font(n->sub_lbl, F_SM, 0);
    lv_obj_set_style_text_color(n->sub_lbl, COL_SUB, 0);
    lv_obj_align(n->sub_lbl, LV_ALIGN_CENTER, 0, 36);
}

static void node_set_arc_pct(node_t *n, float pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(n->arc, (int32_t)(pct * 10));   /* 0..1000 */
}

/* ---------- Animated dotted flow lines ---------- */

/* Build a flow line from corner node center -> house center. The endpoints are
 * pulled in to the two circle edges so dots travel only the gap between them. */
static void flow_build(int idx, lv_obj_t *parent, int ncx, int ncy, lv_color_t color)
{
    flow_t *f = &s_flow[idx];
    f->color = color;
    f->dir   = 0;

    float dx = (float)(HOUSE_CX - ncx), dy = (float)(HOUSE_CY - ncy);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) len = 1;
    float ux = dx / len, uy = dy / len;
    f->ax = ncx + (int)(ux * (NODE_DIA / 2 + 4));
    f->ay = ncy + (int)(uy * (NODE_DIA / 2 + 4));
    f->bx = HOUSE_CX - (int)(ux * (HOUSE_DIA / 2 + 4));
    f->by = HOUSE_CY - (int)(uy * (HOUSE_DIA / 2 + 4));

    /* Faint static guide line underneath the moving dots. */
    f->lpts[0].x = f->ax; f->lpts[0].y = f->ay;
    f->lpts[1].x = f->bx; f->lpts[1].y = f->by;
    f->line = lv_line_create(parent);
    lv_line_set_points(f->line, f->lpts, 2);
    lv_obj_set_style_line_color(f->line, COL_ARC_BG, 0);
    lv_obj_set_style_line_width(f->line, 2, 0);
    lv_obj_set_style_line_opa(f->line, LV_OPA_50, 0);
    lv_obj_set_style_line_rounded(f->line, true, 0);

    for (int i = 0; i < FLOW_DOTS; i++) {
        lv_obj_t *d = lv_obj_create(parent);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, FLOW_DOT_DIA, FLOW_DOT_DIA);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, color, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        f->dot[i] = d;
    }
}

static void flow_set_dir(int idx, int dir)
{
    if (idx >= 0 && idx < FLOW_LINES) s_flow[idx].dir = dir;
}

/* Smooth Hermite ease (0..1) -- softens the dot fade at both line ends. */
static float smoothstep01(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* ~25 fps: advance the phase and position every dot along its line in the
 * direction of power flow. Each dot eases in (small + faint) as it leaves the
 * source, runs full near the middle, and dissolves as it reaches the sink --
 * a soft continuous stream instead of hard popping dots. */
static void flow_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_flow_phase += FLOW_SPEED;
    if (s_flow_phase >= 1.0f) s_flow_phase -= 1.0f;

    for (int i = 0; i < FLOW_LINES; i++) {
        flow_t *f = &s_flow[i];
        if (f->dir == 0) {
            for (int k = 0; k < FLOW_DOTS; k++) lv_obj_add_flag(f->dot[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        for (int k = 0; k < FLOW_DOTS; k++) {
            lv_obj_t *d = f->dot[k];
            float p = s_flow_phase + (float)k / FLOW_DOTS;
            p -= (float)(int)p;                       /* frac, 0..1 */
            float tt = (f->dir > 0) ? p : (1.0f - p); /* travel direction */

            /* Envelope: 0 at the ends, 1 across the middle (both ends fade). */
            float env = 1.0f;
            if (tt < FLOW_FADE)             env = smoothstep01(tt / FLOW_FADE);
            else if (tt > 1.0f - FLOW_FADE) env = smoothstep01((1.0f - tt) / FLOW_FADE);

            int dia = FLOW_DOT_MIN + (int)((FLOW_DOT_DIA - FLOW_DOT_MIN) * env + 0.5f);
            int x = f->ax + (int)((f->bx - f->ax) * tt + 0.5f);
            int y = f->ay + (int)((f->by - f->ay) * tt + 0.5f);

            lv_obj_set_size(d, dia, dia);
            lv_obj_set_style_bg_opa(d, (lv_opa_t)(env * 255.0f), 0);
            lv_obj_set_pos(d, x - dia / 2, y - dia / 2);
            lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---------- Top-center clock & date ---------- */

static const char *WD_DE[7] = {
    "Sonntag", "Montag", "Dienstag", "Mittwoch",
    "Donnerstag", "Freitag", "Samstag"
};

/* Once per second: show local time/date in the top center. Hidden until NTP
 * has delivered a first synced time so we never display the 1970 boot epoch. */
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_clock_lbl) return;

    if (!ntp_is_synced()) {
        lv_obj_add_flag(s_clock_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_date_lbl,  LV_OBJ_FLAG_HIDDEN);
        return;
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_clock_lbl, b);

    char d[48];
    snprintf(d, sizeof(d), "%s, %02d.%02d.%04d",
             WD_DE[tm.tm_wday % 7], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    lv_label_set_text(s_date_lbl, d);

    lv_obj_clear_flag(s_clock_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_date_lbl,  LV_OBJ_FLAG_HIDDEN);
}

/* ---------- Uptime / restart popup (tap the clock or date) ---------- */

static void sys_uptime_text(char *buf, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;   /* us -> s since boot */
    int days = (int)(s / 86400); s %= 86400;
    int hrs  = (int)(s / 3600);  s %= 3600;
    int mins = (int)(s / 60);
    int secs = (int)(s % 60);
    snprintf(buf, n, "%d T  %02d Std  %02d Min  %02d Sek", days, hrs, mins, secs);
}

static void sys_popup_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm_popup) { lv_obj_del(s_confirm_popup); s_confirm_popup = NULL; }
    if (s_sys_timer)     { lv_timer_del(s_sys_timer);   s_sys_timer = NULL; }
    if (s_sys_popup)     { lv_obj_del(s_sys_popup);     s_sys_popup = NULL; }
    s_sys_uptime_lbl = NULL;
}

/* Once per second while the popup is open: refresh the live uptime. */
static void sys_uptime_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_sys_uptime_lbl) return;
    char b[48];
    sys_uptime_text(b, sizeof(b));
    lv_label_set_text(s_sys_uptime_lbl, b);
}

static void restart_do_cb(lv_event_t *e)
{
    (void)e;
    esp_restart();   /* hard reboot; no return */
}

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm_popup) { lv_obj_del(s_confirm_popup); s_confirm_popup = NULL; }
}

/* Second-level "really restart?" overlay on top of the system popup. */
static void restart_ask_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm_popup) return;

    s_confirm_popup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_confirm_popup);
    lv_obj_set_size(s_confirm_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_confirm_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm_popup, LV_OPA_70, 0);
    lv_obj_add_flag(s_confirm_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_confirm_popup, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_confirm_popup);
    lv_obj_set_width(card, 560);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_ARC_BG, 0);
    lv_obj_set_style_border_color(card, COL_SUB, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* absorb taps (don't cancel) */

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  Neustart?");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_set_style_text_color(title, COL_ERR, 0);

    lv_obj_t *txt = lv_label_create(card);
    lv_label_set_text(txt, "Gerät jetzt wirklich neu starten?");
    lv_obj_set_style_text_font(txt, F_MD, 0);
    lv_obj_set_style_text_color(txt, COL_TEXT, 0);

    lv_obj_t *act = lv_obj_create(card);
    lv_obj_remove_style_all(act);
    lv_obj_set_width(act, LV_PCT(100));
    lv_obj_set_height(act, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(act, 12, 0);
    lv_obj_set_style_margin_top(act, 4, 0);
    lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *yes = lv_button_create(act);
    lv_obj_set_flex_grow(yes, 1);
    lv_obj_set_height(yes, 52);
    lv_obj_set_style_bg_color(yes, COL_ERR, 0);
    lv_obj_add_event_cb(yes, restart_do_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, LV_SYMBOL_REFRESH "  Neu starten");
    lv_obj_set_style_text_font(yl, F_MD, 0);
    lv_obj_set_style_text_color(yl, lv_color_black(), 0);
    lv_obj_center(yl);

    lv_obj_t *no = lv_button_create(act);
    lv_obj_set_flex_grow(no, 1);
    lv_obj_set_height(no, 52);
    lv_obj_set_style_bg_color(no, COL_BTN, 0);
    lv_obj_add_event_cb(no, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, LV_SYMBOL_CLOSE "  Abbrechen");
    lv_obj_set_style_text_font(nl, F_MD, 0);
    lv_obj_center(nl);
}

/* Tapping the clock/date opens the system popup: live uptime + restart. */
static void sys_popup_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_sys_popup) return;

    s_sys_popup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_sys_popup);
    lv_obj_set_size(s_sys_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sys_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sys_popup, LV_OPA_60, 0);
    lv_obj_add_flag(s_sys_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sys_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sys_popup, sys_popup_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_sys_popup);
    lv_obj_set_width(card, 600);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_ARC_BG, 0);
    lv_obj_set_style_border_color(card, COL_SUB, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* absorb taps (don't close) */

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_POWER "  System");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_set_style_text_color(title, COL_HOUSE, 0);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, "Laufzeit seit Neustart");
    lv_obj_set_style_text_font(cap, F_MD, 0);
    lv_obj_set_style_text_color(cap, COL_SUB, 0);

    s_sys_uptime_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_sys_uptime_lbl, F_LG, 0);
    lv_obj_set_style_text_color(s_sys_uptime_lbl, COL_TEXT, 0);
    char b[48];
    sys_uptime_text(b, sizeof(b));
    lv_label_set_text(s_sys_uptime_lbl, b);
    s_sys_timer = lv_timer_create(sys_uptime_timer_cb, 1000, NULL);

    /* Action row: Restart (red) + Close (blue). */
    lv_obj_t *act = lv_obj_create(card);
    lv_obj_remove_style_all(act);
    lv_obj_set_width(act, LV_PCT(100));
    lv_obj_set_height(act, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(act, 12, 0);
    lv_obj_set_style_margin_top(act, 8, 0);
    lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *rst = lv_button_create(act);
    lv_obj_set_flex_grow(rst, 1);
    lv_obj_set_height(rst, 52);
    lv_obj_set_style_bg_color(rst, COL_ERR, 0);
    lv_obj_add_event_cb(rst, restart_ask_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rst);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  Neustart");
    lv_obj_set_style_text_font(rl, F_MD, 0);
    lv_obj_set_style_text_color(rl, lv_color_black(), 0);
    lv_obj_center(rl);

    lv_obj_t *close = lv_button_create(act);
    lv_obj_set_flex_grow(close, 1);
    lv_obj_set_height(close, 52);
    lv_obj_set_style_bg_color(close, COL_BTN, 0);
    lv_obj_add_event_cb(close, sys_popup_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE "  Schließen");
    lv_obj_set_style_text_font(cl, F_MD, 0);
    lv_obj_center(cl);
}

/* ---------------------------------------------------------------- */

/* Polls the WiFi manager and reflects the connection state in the top-left
 * status badge. Runs on the LVGL task, so it may touch LVGL objects. */
static void wifi_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_wifi_lbl) return;

    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);

    char       buf[48];
    lv_color_t col;
    switch (st.state) {
    case WIFI_MGR_STA_CONNECTED:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %s",
                 st.sta_ssid[0] ? st.sta_ssid : "WLAN");
        col = COL_BYD;          /* green */
        break;
    case WIFI_MGR_STA_CONNECTING:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  verbinde...");
        col = COL_PV;           /* amber */
        break;
    case WIFI_MGR_AP_FALLBACK:
    case WIFI_MGR_AP_ONLY:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  AP: %s",
                 st.ap_ssid[0] ? st.ap_ssid : "aktiv");
        col = COL_HOUSE;        /* blue */
        break;
    default:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  --");
        col = COL_SUB;
        break;
    }
    lv_label_set_text(s_wifi_lbl, buf);
    lv_obj_set_style_text_color(s_wifi_lbl, col, 0);
}

static const char *wifi_state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_STA_CONNECTED:  return "Verbunden";
    case WIFI_MGR_STA_CONNECTING: return "Verbinde...";
    case WIFI_MGR_AP_FALLBACK:    return "AP-Fallback";
    case WIFI_MGR_AP_ONLY:        return "AP-Modus";
    default:                      return "Startet...";
    }
}

static void wifi_popup_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_popup) {
        lv_obj_del(s_wifi_popup);
        s_wifi_popup = NULL;
    }
}

static void wifi_popup_kv(lv_obj_t *parent, const char *key, const char *val)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_width(k, 180);
    lv_obj_set_style_text_color(k, COL_SUB, 0);
    lv_obj_set_style_text_font(k, F_MD, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, (val && val[0]) ? val : "-");
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_obj_set_style_text_font(v, F_MD, 0);
}

/* Tapping the WiFi badge opens a modal with the connection details. */
static void wifi_badge_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_popup) return;

    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);

    s_wifi_popup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_wifi_popup);
    lv_obj_set_size(s_wifi_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_wifi_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wifi_popup, LV_OPA_60, 0);
    lv_obj_add_flag(s_wifi_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wifi_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_wifi_popup, wifi_popup_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_wifi_popup);
    lv_obj_set_size(card, 640, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, 452, 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_ARC_BG, 0);
    lv_obj_set_style_border_color(card, COL_SUB, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* absorb taps (don't close) */

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WLAN-Status");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);

    char rssi[16] = { 0 };
    if (st.state == WIFI_MGR_STA_CONNECTED && st.rssi) {
        snprintf(rssi, sizeof(rssi), "%d dBm", st.rssi);
    }

    wifi_popup_kv(card, "Status",  wifi_state_str(st.state));
    wifi_popup_kv(card, "SSID",    st.sta_ssid);
    wifi_popup_kv(card, "IP",      st.sta_ip);
    wifi_popup_kv(card, "Gateway", st.sta_gw);
    wifi_popup_kv(card, "MAC",     st.mac);
    wifi_popup_kv(card, "Signal",  rssi);
    if (st.ap_active) {
        wifi_popup_kv(card, "AP-SSID", st.ap_ssid);
        wifi_popup_kv(card, "AP-IP",   st.ap_ip);
    }

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, LV_PCT(100), 52);
    lv_obj_set_style_margin_top(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, COL_BTN, 0);
    lv_obj_add_event_cb(btn, wifi_popup_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE "  Schließen");
    lv_obj_set_style_text_font(bl, F_MD, 0);
    lv_obj_center(bl);
}

/* ---- Display standby: blank the backlight after N s of no touch ---- */
static uint32_t s_sleep_ms;
static bool     s_asleep;

static void sleep_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_sleep_ms == 0) return;                       /* disabled */
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s_asleep && idle >= s_sleep_ms) {
        display_backlight(false);                      /* sleep */
        s_asleep = true;
    } else if (s_asleep && idle < s_sleep_ms) {
        display_backlight(true);                       /* touch woke it */
        s_asleep = false;
    }
}

void ui_flow_set_sleep_timeout(uint32_t seconds)
{
    s_sleep_ms = seconds * 1000;
    if (s_sleep_ms == 0 && s_asleep) {                 /* disabled -> wake */
        display_backlight(true);
        s_asleep = false;
    }
}

/* Software "contrast": a full-screen grey wash on the top layer. 100 % = no
 * wash (full contrast); lower values blend toward grey, lowering contrast.
 * Lives on lv_layer_top() so it persists across the flow/settings screens. */
void ui_flow_set_contrast(uint8_t pct)
{
    static lv_obj_t *s_ov;
    if (pct > 100) pct = 100;
    if (!s_ov) {
        s_ov = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_ov);
        lv_obj_set_size(s_ov, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_ov, lv_color_hex(0x808080), 0);
        lv_obj_clear_flag(s_ov, LV_OBJ_FLAG_CLICKABLE);   /* touch passes through */
        lv_obj_clear_flag(s_ov, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_opa_t opa = (lv_opa_t)((100 - pct) * 160 / 100);   /* 0..160 */
    lv_obj_set_style_bg_opa(s_ov, opa, 0);
    if (opa == 0) lv_obj_add_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- "Erzeugung" drill-down: individual inverters ---------- */

static void gen_popup_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_gen_popup) { lv_obj_del(s_gen_popup); s_gen_popup = NULL; }
}

static void gen_node_cb(lv_event_t *e)
{
    (void)e;
    if (s_gen_popup) return;

    mb_dev_live_t dl[MB_MAX_DEVICES];
    int n = modbus_tcp_get_device_live(dl, MB_MAX_DEVICES);

    s_gen_popup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_gen_popup);
    lv_obj_set_size(s_gen_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_gen_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_gen_popup, LV_OPA_60, 0);
    lv_obj_add_flag(s_gen_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_gen_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_gen_popup, gen_popup_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_gen_popup);
    lv_obj_set_width(card, 640);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, 452, 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_ARC_BG, 0);
    lv_obj_set_style_border_color(card, COL_SUB, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* absorb taps */

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_CHARGE "  Erzeugung");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_set_style_text_color(title, COL_PV, 0);

    float total = 0;
    int shown = 0;
    for (int i = 0; i < n; i++) {
        bool producer = (dl[i].role == MB_ROLE_INVERTER) ||
                        (dl[i].role == MB_ROLE_PRODMETER);
        if (!producer) continue;
        shown++;
        if (dl[i].connected) total += dl[i].pv_w;

        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row);
        if (dl[i].name[0])
            lv_label_set_text_fmt(name, "%s  (%s)", dl[i].name, dl[i].ip);
        else
            lv_label_set_text_fmt(name, "%s  %s  (ID %u)",
                modbus_tcp_mfr_name(dl[i].mfr), dl[i].ip, dl[i].slave);
        lv_obj_set_width(name, 420);
        lv_obj_set_style_text_font(name, F_MD, 0);
        lv_obj_set_style_text_color(name, dl[i].connected ? COL_TEXT : COL_SUB, 0);

        lv_obj_t *val = lv_label_create(row);
        if (dl[i].connected) {
            char vb[24];
            snprintf(vb, sizeof(vb), "%.0f W", dl[i].pv_w);   /* C-lib %f */
            lv_label_set_text(val, vb);
        } else {
            lv_label_set_text(val, "offline");
        }
        lv_obj_set_style_text_font(val, F_MD, 0);
        lv_obj_set_style_text_color(val, dl[i].connected ? COL_BYD : COL_ERR, 0);
    }

    if (shown == 0) {
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, "Keine Wechselrichter konfiguriert.\n"
                             "Einstellungen -> Mod TCP -> '+ Gerät'.");
        lv_obj_set_style_text_color(l, COL_SUB, 0);
    } else {
        lv_obj_t *tot = lv_label_create(card);
        char tb[32];
        snprintf(tb, sizeof(tb), "Gesamt: %.2f kW", total / 1000.0f);
        lv_label_set_text(tot, tb);
        lv_obj_set_style_text_font(tot, F_MD, 0);
        lv_obj_set_style_text_color(tot, COL_TEXT, 0);
    }

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, LV_PCT(100), 52);
    lv_obj_set_style_margin_top(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, COL_BTN, 0);
    lv_obj_add_event_cb(btn, gen_popup_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_CLOSE "  Schließen");
    lv_obj_set_style_text_font(bl, F_MD, 0);
    lv_obj_center(bl);
}

/* ---------- Deye battery-mode popup (Forced-Charge / -Discharge / Normal) ---- */

static const char *DEYE_MODE_CAP[DEYE_MODE_COUNT] = {
    [DEYE_MODE_NORMAL]         = "Normal",
    [DEYE_MODE_FORCE_CHARGE]   = LV_SYMBOL_DOWNLOAD "  Laden",
    [DEYE_MODE_FORCE_DISCHARGE]= LV_SYMBOL_UPLOAD   "  Entladen",
};

static void deye_popup_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_deye_popup) { lv_obj_del(s_deye_popup); s_deye_popup = NULL; }
    s_deye_pwr_row = s_deye_pwr_lbl = s_deye_pwr_slider = NULL;
}

/* Reflect the selected mode: highlight its button and show the power slider
 * only for the two forced modes (Normal needs no power). */
static void deye_refresh_sel(void)
{
    for (int i = 0; i < DEYE_MODE_COUNT; i++) {
        if (!s_deye_mode_btn[i]) continue;
        lv_obj_set_style_bg_color(s_deye_mode_btn[i],
            (i == (int)s_deye_sel_mode) ? COL_BTN : COL_PANEL2, 0);
    }
    if (s_deye_pwr_row) {
        if (s_deye_sel_mode == DEYE_MODE_NORMAL)
            lv_obj_add_flag(s_deye_pwr_row, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_deye_pwr_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void deye_mode_btn_cb(lv_event_t *e)
{
    deye_mode_t m = (deye_mode_t)(intptr_t)lv_event_get_user_data(e);
    s_deye_sel_mode = m;
    deye_refresh_sel();
    if (m == DEYE_MODE_NORMAL) {
        deye_ctrl_apply(DEYE_MODE_NORMAL, 0);
        deye_popup_close_cb(NULL);
    }
}

static void deye_pwr_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    v = ((v + 50) / 100) * 100;                       /* snap to 100 W */
    if (s_deye_pwr_lbl) {
        char b[24];
        snprintf(b, sizeof(b), "%d W", v);
        lv_label_set_text(s_deye_pwr_lbl, b);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
}

static void deye_apply_cb(lv_event_t *e)
{
    (void)e;
    int power = s_deye_pwr_slider ? lv_slider_get_value(s_deye_pwr_slider) : 0;
    power = ((power + 50) / 100) * 100;
    deye_ctrl_apply(s_deye_sel_mode, power);
    deye_popup_close_cb(NULL);
}

/* Tapping the Deye battery node opens the battery-mode chooser. */
static void deye_node_cb(lv_event_t *e)
{
    (void)e;
    if (s_deye_popup) return;

    s_deye_sel_mode = deye_ctrl_get_mode();

    s_deye_popup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_deye_popup);
    lv_obj_set_size(s_deye_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_deye_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_deye_popup, LV_OPA_60, 0);
    lv_obj_add_flag(s_deye_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_deye_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_deye_popup, deye_popup_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_deye_popup);
    lv_obj_set_width(card, 640);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, 452, 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_ARC_BG, 0);
    lv_obj_set_style_border_color(card, COL_SUB, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* absorb taps (don't close) */

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_BATTERY_FULL "  Batterie Deye - Modus");
    lv_obj_set_style_text_font(title, F_LG, 0);
    lv_obj_set_style_text_color(title, COL_DEYE, 0);

    /* Three mode buttons in a row. */
    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < DEYE_MODE_COUNT; i++) {
        lv_obj_t *b = lv_button_create(btn_row);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 64);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, COL_PANEL2, 0);
        lv_obj_add_event_cb(b, deye_mode_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, DEYE_MODE_CAP[i]);
        lv_obj_set_style_text_font(l, F_MD, 0);
        lv_obj_set_style_text_color(l, COL_TEXT, 0);
        lv_obj_center(l);
        s_deye_mode_btn[i] = b;
    }

    /* Power block: "Leistung" caption + live value + slider (forced modes only). */
    s_deye_pwr_row = lv_obj_create(card);
    lv_obj_remove_style_all(s_deye_pwr_row);
    lv_obj_set_width(s_deye_pwr_row, LV_PCT(100));
    lv_obj_set_height(s_deye_pwr_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_deye_pwr_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_deye_pwr_row, 8, 0);
    lv_obj_clear_flag(s_deye_pwr_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prow = lv_obj_create(s_deye_pwr_row);
    lv_obj_remove_style_all(prow);
    lv_obj_set_width(prow, LV_PCT(100));
    lv_obj_set_height(prow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pcap = lv_label_create(prow);
    lv_label_set_text(pcap, "Leistung");
    lv_obj_set_style_text_font(pcap, F_MD, 0);
    lv_obj_set_style_text_color(pcap, COL_SUB, 0);

    s_deye_pwr_lbl = lv_label_create(prow);
    lv_obj_set_style_text_font(s_deye_pwr_lbl, F_MD, 0);
    lv_obj_set_style_text_color(s_deye_pwr_lbl, COL_TEXT, 0);

    int pw = deye_ctrl_get_power();
    s_deye_pwr_slider = lv_slider_create(s_deye_pwr_row);
    lv_obj_set_width(s_deye_pwr_slider, LV_PCT(100));
    lv_obj_set_height(s_deye_pwr_slider, 16);
    lv_slider_set_range(s_deye_pwr_slider, 0, DEYE_FORCE_MAX_W);
    lv_slider_set_value(s_deye_pwr_slider, pw, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_deye_pwr_slider, COL_PANEL2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_deye_pwr_slider, COL_BTN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_deye_pwr_slider, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_radius(s_deye_pwr_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_deye_pwr_slider, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_deye_pwr_slider, COL_BTN, LV_PART_KNOB);
    lv_obj_add_event_cb(s_deye_pwr_slider, deye_pwr_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_deye_pwr_slider, deye_pwr_cb, LV_EVENT_RELEASED, NULL);
    {
        char b[24];
        snprintf(b, sizeof(b), "%d W", pw);
        lv_label_set_text(s_deye_pwr_lbl, b);
    }

    /* Action row: Apply (green) + Close (blue). */
    lv_obj_t *act = lv_obj_create(card);
    lv_obj_remove_style_all(act);
    lv_obj_set_width(act, LV_PCT(100));
    lv_obj_set_height(act, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(act, 12, 0);
    lv_obj_set_style_margin_top(act, 4, 0);
    lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *apply = lv_button_create(act);
    lv_obj_set_flex_grow(apply, 1);
    lv_obj_set_height(apply, 52);
    lv_obj_set_style_bg_color(apply, COL_OK, 0);
    lv_obj_add_event_cb(apply, deye_apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(apply);
    lv_label_set_text(al, LV_SYMBOL_OK "  Übernehmen");
    lv_obj_set_style_text_font(al, F_MD, 0);
    lv_obj_set_style_text_color(al, lv_color_black(), 0);   /* black-on-green */
    lv_obj_center(al);

    lv_obj_t *close = lv_button_create(act);
    lv_obj_set_flex_grow(close, 1);
    lv_obj_set_height(close, 52);
    lv_obj_set_style_bg_color(close, COL_BTN, 0);
    lv_obj_add_event_cb(close, deye_popup_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE "  Schließen");
    lv_obj_set_style_text_font(cl, F_MD, 0);
    lv_obj_center(cl);

    deye_refresh_sel();
}

void ui_flow_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_color(scr, COL_TEXT, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* WiFi status badge (top-left), refreshed every 2 s by a timer. */
    s_wifi_lbl = lv_label_create(scr);
    lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI "  --");
    lv_obj_set_style_text_font(s_wifi_lbl, F_MD, 0);
    lv_obj_set_style_text_color(s_wifi_lbl, COL_SUB, 0);
    lv_obj_align(s_wifi_lbl, LV_ALIGN_TOP_LEFT, 14, 16);
    lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_wifi_lbl, 14);
    lv_obj_add_event_cb(s_wifi_lbl, wifi_badge_cb, LV_EVENT_CLICKED, NULL);
    lv_timer_create(wifi_status_timer_cb, 2000, NULL);

    /* Settings gear button (top-right corner). */
    lv_obj_t *gear = lv_button_create(scr);
    lv_obj_set_size(gear, 72, 72);                 /* 50% larger than before */
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(gear, COL_ARC_BG, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_set_style_border_width(gear, 0, 0);
    lv_obj_add_event_cb(gear, settings_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gear_icon = lv_label_create(gear);
    lv_label_set_text(gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_icon, COL_TEXT, 0);
    lv_obj_set_style_text_font(gear_icon, F_LG, 0);
    lv_obj_center(gear_icon);

    /* Clock + date (top center). Hidden until NTP delivers a synced time;
     * updated once a second by clock_timer_cb. */
    s_clock_lbl = lv_label_create(scr);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_set_style_text_font(s_clock_lbl, F_XL, 0);
    lv_obj_set_style_text_color(s_clock_lbl, COL_TEXT, 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_add_flag(s_clock_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_clock_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_clock_lbl, 14);
    lv_obj_add_event_cb(s_clock_lbl, sys_popup_open_cb, LV_EVENT_CLICKED, NULL);

    s_date_lbl = lv_label_create(scr);
    lv_label_set_text(s_date_lbl, "");
    lv_obj_set_style_text_font(s_date_lbl, F_MD, 0);
    lv_obj_set_style_text_color(s_date_lbl, COL_SUB, 0);
    lv_obj_align(s_date_lbl, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_flag(s_date_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_date_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_date_lbl, 14);
    lv_obj_add_event_cb(s_date_lbl, sys_popup_open_cb, LV_EVENT_CLICKED, NULL);
    lv_timer_create(clock_timer_cb, 1000, NULL);

    /* Corner placement (node_pos idx: 0=TL, 1=TR, 2=BL, 3=BR):
     *   PV  -> top-left,  Netz -> bottom-left,
     *   Deye-> top-right, BYD  -> bottom-right.
     * The flow ARRAY index stays type-bound (0=PV,1=Netz,2=BYD,3=Deye) to match
     * the ui_flow_set_* direction calls; only the corner each occupies changes. */
    #define CORNER_PV    0
    #define CORNER_NETZ  2
    #define CORNER_DEYE  1
    #define CORNER_BYD   3
    int px, py;

    /* Animated dotted flow lines first, so the circles draw over the endpoints. */
    node_pos(CORNER_PV,   &px, &py); flow_build(0, scr, px, py, COL_PV);
    node_pos(CORNER_NETZ, &px, &py); flow_build(1, scr, px, py, COL_GRID);
    node_pos(CORNER_BYD,  &px, &py); flow_build(2, scr, px, py, COL_BYD);
    node_pos(CORNER_DEYE, &px, &py); flow_build(3, scr, px, py, COL_DEYE);

    /* House node in the CENTER (larger). */
    node_build(&s_house, scr, HOUSE_CX, HOUSE_CY, HOUSE_DIA, LV_SYMBOL_HOME, COL_HOUSE);

    /* Corner nodes. */
    node_pos(CORNER_PV,   &px, &py); node_build(&s_pv,   scr, px, py, NODE_DIA, LV_SYMBOL_CHARGE,       COL_PV);
    node_pos(CORNER_NETZ, &px, &py); node_build(&s_grid, scr, px, py, NODE_DIA, LV_SYMBOL_POWER,        COL_GRID);
    node_pos(CORNER_DEYE, &px, &py); node_build(&s_deye, scr, px, py, NODE_DIA, LV_SYMBOL_BATTERY_FULL, COL_DEYE);
    node_pos(CORNER_BYD,  &px, &py); node_build(&s_byd,  scr, px, py, NODE_DIA, LV_SYMBOL_BATTERY_FULL, COL_BYD);

    /* Transparent hit area over the PV node -> per-inverter "Erzeugung" popup
     * (a separate object so a tap can't drag the arc gauge value). */
    node_pos(CORNER_PV, &px, &py);
    lv_obj_t *pvhit = lv_button_create(scr);
    lv_obj_remove_style_all(pvhit);
    lv_obj_set_size(pvhit, NODE_DIA, NODE_DIA);
    lv_obj_set_pos(pvhit, px - NODE_DIA / 2, py - NODE_DIA / 2);
    lv_obj_set_style_radius(pvhit, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(pvhit, gen_node_cb, LV_EVENT_CLICKED, NULL);

    /* Transparent hit area over the Deye node -> battery-mode chooser popup. */
    node_pos(CORNER_DEYE, &px, &py);
    lv_obj_t *deyehit = lv_button_create(scr);
    lv_obj_remove_style_all(deyehit);
    lv_obj_set_size(deyehit, NODE_DIA, NODE_DIA);
    lv_obj_set_pos(deyehit, px - NODE_DIA / 2, py - NODE_DIA / 2);
    lv_obj_set_style_radius(deyehit, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(deyehit, deye_node_cb, LV_EVENT_CLICKED, NULL);

    /* Caption labels under the four corner nodes. */
    struct { node_t *n; const char *cap; } corner[4] = {
        { &s_pv,   "PV-Erzeugung" },
        { &s_grid, "Netz" },
        { &s_deye, "Batterie Deye" },
        { &s_byd,  "Batterie BYD" },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *cap = lv_label_create(scr);
        lv_label_set_text(cap, corner[i].cap);
        lv_obj_set_style_text_font(cap, F_SM, 0);
        lv_obj_set_style_text_color(cap, COL_SUB, 0);
        lv_obj_align_to(cap, corner[i].n->arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    }

    /* Drive the flow animation (~30 fps for a smooth, fine stream). */
    lv_timer_create(flow_timer_cb, 33, NULL);

    /* Full-height vertical grid-setpoint slider on the left edge. The Eastron
     * emulation reports (real grid - setpoint) so the Deye regulates the real
     * grid point to this value (zero-export trick). */
    {
        /* Slider fills most of the height; the round knob overshoots the track
         * ends a little, so the track is inset from the WiFi badge (top) and
         * the bottom label block (caption + value) far enough to never touch
         * them at the extremes. Build label now lives bottom-right. */
        /* Read the persisted setpoint straight from NVS: ui_flow_create() runs
         * before modbus_rtu_start(), so modbus_rtu's cached value isn't loaded
         * yet -- NVS is the source of truth and survives reboots. */
        int sp_val = nvs_store_get_grid_sp();

        lv_obj_t *sp = lv_slider_create(scr);
        lv_obj_set_size(sp, 16, 300);                    /* same thickness as bri/con */
        lv_obj_align(sp, LV_ALIGN_TOP_LEFT, 22, 78);
        lv_slider_set_range(sp, -1000, 1000);
        lv_slider_set_value(sp, sp_val, LV_ANIM_OFF);
        /* Identical style to the brightness/contrast sliders. */
        lv_obj_set_style_bg_color(sp, COL_PANEL2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sp, COL_BTN, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sp, COL_TEXT, LV_PART_KNOB);        /* white knob */
        /* Vertical capsule knob (taller than wide), elongated along the
         * slider's travel axis -- not a round dot, not a horizontal bar. */
        lv_obj_set_style_pad_hor(sp, 3,  LV_PART_KNOB);
        lv_obj_set_style_pad_ver(sp, 16, LV_PART_KNOB);
        lv_obj_set_style_radius(sp, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_border_width(sp, 3, LV_PART_KNOB);
        lv_obj_set_style_border_color(sp, COL_BTN, LV_PART_KNOB);
        lv_obj_add_event_cb(sp, grid_sp_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sp, grid_sp_cb, LV_EVENT_RELEASED, NULL);

        /* Caption + value, stacked at the bottom-left:  "Netz-Soll" / "+500 W". */
        lv_obj_t *cap = lv_label_create(scr);
        lv_label_set_text(cap, "Netz-Soll");
        lv_obj_set_style_text_font(cap, F_MD, 0);
        lv_obj_set_style_text_color(cap, COL_SUB, 0);
        lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 6, 428);

        s_sp_lbl = lv_label_create(scr);
        lv_obj_set_style_text_font(s_sp_lbl, F_MD, 0);
        lv_obj_set_style_text_color(s_sp_lbl, COL_TEXT, 0);
        lv_obj_align(s_sp_lbl, LV_ALIGN_TOP_LEFT, 6, 452);
        char b[24];
        snprintf(b, sizeof(b), "%+d W", sp_val);
        lv_label_set_text(s_sp_lbl, b);
    }

    /* Build-number badge (bottom-left). Shows the firmware build compiled in
     * from build_info.h; ui_flow_set_fs_build() later colours it by whether
     * the flashed filesystem image carries the same number. */
    s_build_lbl = lv_label_create(scr);
    lv_label_set_text(s_build_lbl, DEYE_BUILD_VERSION);
    lv_obj_set_style_text_font(s_build_lbl, F_SM, 0);
    lv_obj_set_style_text_color(s_build_lbl, COL_SUB, 0);
    lv_obj_align(s_build_lbl, LV_ALIGN_BOTTOM_RIGHT, -6, -4);

    /* No placeholder seeds: nodes show "--" until live Modbus data arrives. */

    /* Apply the saved display contrast (brightness is applied in display_init). */
    ui_flow_set_contrast(nvs_store_get_contrast());

    /* Standby: poll inactivity twice a second and blank/restore the backlight. */
    ui_flow_set_sleep_timeout(nvs_store_get_sleep_secs());
    lv_timer_create(sleep_timer_cb, 500, NULL);
}

void ui_flow_set_fs_build(int fs_build)
{
    if (!s_build_lbl) {
        return;
    }
    int fw = DEYE_BUILD_NUMBER;
    if (fs_build < 0) {
        /* No asset image flashed -- can't confirm, but FW/UI still agree. */
        lv_label_set_text_fmt(s_build_lbl, "%s  " LV_SYMBOL_WARNING " FS?",
                              DEYE_BUILD_VERSION);
        lv_obj_set_style_text_color(s_build_lbl, COL_SUB, 0);
    } else if (fs_build == fw) {
        lv_label_set_text_fmt(s_build_lbl, "%s  " LV_SYMBOL_OK,
                              DEYE_BUILD_VERSION, fw);
        lv_obj_set_style_text_color(s_build_lbl, COL_OK, 0);
    } else {
        lv_label_set_text_fmt(s_build_lbl,
                              LV_SYMBOL_WARNING " FW #%d / FS #%d", fw, fs_build);
        lv_obj_set_style_text_color(s_build_lbl, COL_ERR, 0);
    }
}

/* ---------------------------------------------------------------- */

void ui_flow_set_pv(float kw)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f kW", kw);
    lv_label_set_text(s_pv.val_lbl, buf);
    node_set_arc_pct(&s_pv, kw / PV_MAX_KW * 100.0f);
    flow_set_dir(0, kw > 0.05f ? +1 : 0);          /* PV always feeds the house */
}

void ui_flow_set_house(float kw)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f kW", kw);
    lv_label_set_text(s_house.val_lbl, buf);
    node_set_arc_pct(&s_house, kw / HOUSE_MAX_KW * 100.0f);
}

void ui_flow_set_grid(float kw_signed)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%s%.1f kW",
             kw_signed >= 0 ? "+" : "-", fabsf(kw_signed));
    lv_label_set_text(s_grid.val_lbl, buf);
    lv_label_set_text(s_grid.sub_lbl, kw_signed >= 0 ? "Bezug" : "Einspeisung");
    node_set_arc_pct(&s_grid, fabsf(kw_signed) / GRID_MAX_KW * 100.0f);
    /* +import: grid -> house; -export: house -> grid. */
    flow_set_dir(1, fabsf(kw_signed) < 0.05f ? 0 : (kw_signed > 0 ? +1 : -1));
}

static void set_battery(node_t *n, int flow_idx, float kw_signed, float soc_pct)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f %%", soc_pct);
    lv_label_set_text(n->val_lbl, buf);
    if (fabsf(kw_signed) < 0.05f) {
        lv_label_set_text(n->sub_lbl, "idle");
    } else {
        /* Display convention: + = charging, - = discharging (kw_signed is the
         * internal +discharge/-charge balance value, so negate for display). */
        float disp = -kw_signed;
        snprintf(buf, sizeof(buf), "%s%.1f kW",
                 disp >= 0 ? "+" : "-", fabsf(disp));
        lv_label_set_text(n->sub_lbl, buf);
    }
    node_set_arc_pct(n, soc_pct);
    /* Flow uses the physical convention: +discharge -> battery feeds the house;
     * -charge -> house charges the battery. */
    flow_set_dir(flow_idx, fabsf(kw_signed) < 0.05f ? 0 : (kw_signed > 0 ? +1 : -1));
}

void ui_flow_set_byd(float kw_signed, float soc_pct)
{
    set_battery(&s_byd, 2, kw_signed, soc_pct);
}

void ui_flow_set_deye(float kw_signed, float soc_pct)
{
    set_battery(&s_deye, 3, kw_signed, soc_pct);
}

/* Reset a node to the "--" placeholder (no live source). flow_idx < 0 = no flow
 * line (the central house node). Avoids ghosting the last value when a device is
 * disabled/removed and the aggregator stops feeding that node. */
static void node_clear(node_t *n, int flow_idx)
{
    lv_label_set_text(n->val_lbl, "--");
    lv_label_set_text(n->sub_lbl, "");
    node_set_arc_pct(n, 0);
    if (flow_idx >= 0) flow_set_dir(flow_idx, 0);
}

void ui_flow_clear_pv(void)    { node_clear(&s_pv,    0); }
void ui_flow_clear_grid(void)  { node_clear(&s_grid,  1); }
void ui_flow_clear_byd(void)   { node_clear(&s_byd,   2); }
void ui_flow_clear_deye(void)  { node_clear(&s_deye,  3); }
void ui_flow_clear_house(void) { node_clear(&s_house, -1); }

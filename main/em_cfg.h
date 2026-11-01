#pragma once

/* Settings of the energy manager: where the house is, which PV planes it has,
 * how big the batteries are, and the Tibber token for the price feed.
 * Entered on the "Energie" tab of /deye; nothing here steers anything yet --
 * the price feed, the PV forecast and the planner read it.
 *
 * Stored as one NVS blob (namespace "em", key "cfg") and part of the settings
 * backup like every other blob. Fields are only ever appended. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define EM_MAX_PLANES     6
#define EM_CFG_VER        1
#define EM_TOKEN_MAX      96       /* Tibber tokens are ~43 chars */

/* Which measurement belongs to a plane, for calibrating its forecast. */
#define EM_METER_NONE     (-1)     /* not measured on its own            */
#define EM_METER_DEYE     99       /* the Deye's own PV strings          */
                                   /* 0..7: index of a Modbus-TCP device */

#define EM_PLANE_AC_MAX_W 60000

typedef struct {
    char     name[16];
    float    kwp;          /* peak power, kWp                              */
    int16_t  tilt;         /* degrees from horizontal, 0..90               */
    int16_t  azimuth;      /* compass degrees: 0 N, 90 E, 180 S, 270 W     */
    int8_t   meter;        /* EM_METER_*                                   */
    uint8_t  _rsv;
    uint16_t ac_w;         /* inverter AC limit, W; 0 = none. In what used to
                            * be reserved bytes: the record keeps its size and
                            * older firmware reads the plane unchanged.      */
} em_plane_t;
_Static_assert(sizeof(em_plane_t) == 28, "em_plane_t is stored in NVS");

typedef struct {
    uint16_t   ver;                     /* EM_CFG_VER                          */
    uint8_t    nplanes;                 /* 0..EM_MAX_PLANES                    */
    uint8_t    _rsv0;
    float      lat, lon;                /* degrees; both 0 = not set           */
    em_plane_t plane[EM_MAX_PLANES];
    float      deye_kwh, byd_kwh;       /* usable capacity, 0 = unknown        */
    uint8_t    byd_reserve_pct;         /* never discharge the BYD below this  */
    uint8_t    byd_max_soc;             /* upper charge limit of the BYD, enforced by
                                         * the ESP (fronius_storage); 0 = off. In a
                                         * byte that used to be reserved: older
                                         * records read as "off".                 */
    uint8_t    _rsv1[2];
    char       tibber_token[EM_TOKEN_MAX];   /* "" = none; never sent out      */

    /* ---- planner (em_planner.h) ---- */
    float      charge_kw;               /* grid-charge power of the Deye, kW   */
    float      min_spread_ct;           /* advantage a charge must bring, ct/kWh
                                         * after losses and cycle cost          */
    float      cycle_cost_ct;           /* battery wear per kWh cycled, ct      */
    uint8_t    soc_min_pct;             /* Deye: never planned below this       */
    uint8_t    soc_max_pct;             /* Deye: grid charge never above this   */
    uint8_t    mode;                    /* EM_MODE_*                            */
    uint8_t    byd_ctrl;                /* 1 = may block the BYD's discharge    */
} em_cfg_t;

/* What the energy manager does with its plan. */
#define EM_MODE_OFF     0   /* nothing: no plan, no action                      */
#define EM_MODE_SHADOW  1   /* plan and show what it would do, act on nothing   */
#define EM_MODE_AUTO    2   /* act on the plan                                   */

/* Defaults for a record that never set them (all 0). */
#define EM_DEF_CHARGE_KW     3.0f
#define EM_DEF_MIN_SPREAD    3.0f
#define EM_DEF_CYCLE_COST    2.0f
#define EM_DEF_SOC_MIN       15
#define EM_DEF_SOC_MAX       95

/* Load from NVS. Call once at start. */
void      em_cfg_init(void);

/* Current settings (a copy). */
void      em_cfg_get(em_cfg_t *out);

/* Validate, store and take over everything EXCEPT the token, which keeps its
 * value (it comes in separately, see em_cfg_set_token). On refusal nothing
 * changes and err holds the reason, in German, for the page. */
esp_err_t em_cfg_set(const em_cfg_t *in, char *err, size_t errlen);

/* Store the Tibber token; "" deletes it. Only token characters are taken. */
esp_err_t em_cfg_set_token(const char *tok, char *err, size_t errlen);

bool      em_cfg_has_token(void);

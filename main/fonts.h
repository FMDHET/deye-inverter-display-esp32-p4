#pragma once

/* Tiny-TTF fonts rendered from the bundled Montserrat-Medium.ttf so German
 * umlauts render. Each has the matching built-in bitmap montserrat font as a
 * fallback, which carries the FontAwesome LV_SYMBOL_* glyphs the TTF lacks.
 *
 * Sizes mirror the previously used lv_font_montserrat_14 / _22 / _28. Created
 * once in fonts_init() (call after LVGL is up, before building the UI).
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t *F_SM;   /* ~14 px */
extern const lv_font_t *F_MD;   /* ~22 px */
extern const lv_font_t *F_LG;   /* ~28 px */
extern const lv_font_t *F_XL;   /* ~44 px (main-screen clock) */

void fonts_init(void);

#ifdef __cplusplus
}
#endif

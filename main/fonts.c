#include "fonts.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "fonts";

/* Montserrat-Medium.ttf embedded via EMBED_FILES. */
extern const uint8_t mont_ttf_start[] asm("_binary_montserrat_medium_ttf_start");
extern const uint8_t mont_ttf_end[]   asm("_binary_montserrat_medium_ttf_end");

const lv_font_t *F_SM;
const lv_font_t *F_MD;
const lv_font_t *F_LG;
const lv_font_t *F_XL;

static const lv_font_t *mk(int px, const lv_font_t *fallback)
{
    lv_font_t *f = lv_tiny_ttf_create_data(mont_ttf_start,
                                           (size_t)(mont_ttf_end - mont_ttf_start), px);
    if (!f) {
        ESP_LOGW(TAG, "tiny_ttf %dpx failed, using bitmap fallback", px);
        return fallback;
    }
    f->fallback = fallback;              /* symbols come from the bitmap font */
    return f;
}

void fonts_init(void)
{
    F_SM = mk(14, &lv_font_montserrat_14);
    F_MD = mk(22, &lv_font_montserrat_22);
    F_LG = mk(28, &lv_font_montserrat_28);
    F_XL = mk(44, &lv_font_montserrat_28);

    /* Make the tiny-ttf font the THEME DEFAULT so that labels which don't set
     * an explicit font (back button, list items, ...) also get umlauts. */
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        /* Primary = the darker blue used for buttons/sliders elsewhere, so all
         * themed widgets (checkboxes, switches, default sliders) match. */
        lv_theme_t *th = lv_theme_default_init(disp,
                            lv_color_hex(0x2563eb),
                            lv_palette_main(LV_PALETTE_GREY),
                            true, F_SM);
        if (th) lv_display_set_theme(disp, th);
    }
    ESP_LOGI(TAG, "tiny-ttf fonts ready (umlauts enabled)");
}

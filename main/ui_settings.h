#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the settings screen (registered as a secondary lv_screen).
 * Call once at boot, after ui_flow_create(). */
void ui_settings_create(void);

/* Loads the settings screen with a slide-in transition. The flow
 * screen is restored by the "Back" button inside the settings UI. */
void ui_settings_open(void);

/* Inject a key from the web mirror (physical PC keyboard) into the currently
 * focused text field. ASCII char, or 8 = backspace. Caller holds the LVGL lock. */
void ui_settings_web_key(uint32_t key);

/* Clipboard bridge for the web mirror: insert pasted text into the focused
 * text field, and read the focused field's text back out (for copy). Both
 * must be called with the LVGL lock held. */
void ui_settings_web_paste(const char *text);
void ui_settings_web_copy(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

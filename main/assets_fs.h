#pragma once

/* Read-only SPIFFS "storage" partition holding versioned assets.
 *
 * Right now it carries a single file -- build.txt -- whose first line is the
 * build number baked in by scripts/build_number.py. Comparing it against the
 * compiled-in DEYE_BUILD_NUMBER proves the flashed filesystem image matches
 * the running firmware. Future UI assets (fonts, images, config) live here too.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the "storage" partition at /assets (read-only). Safe to call once. */
esp_err_t assets_fs_mount(void);

/* Build number read from /assets/build.txt, or -1 if the FS is absent,
 * unmounted or unreadable. */
int assets_fs_build_number(void);

#ifdef __cplusplus
}
#endif

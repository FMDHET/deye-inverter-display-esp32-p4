#include "assets_fs.h"

#include "esp_log.h"
#include "esp_spiffs.h"

#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "assets_fs";

#define ASSETS_PARTITION  "storage"
#define ASSETS_MOUNT      "/assets"

esp_err_t assets_fs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = ASSETS_MOUNT,
        .partition_label        = ASSETS_PARTITION,
        .max_files              = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        /* Not fatal: the firmware runs fine without the asset image, the UI
         * just reports the FS build as "n/a". */
        ESP_LOGW(TAG, "SPIFFS mount failed (%s) -- assets unavailable",
                 esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(ASSETS_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at %s: %u/%u bytes used",
                 ASSETS_MOUNT, (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

int assets_fs_build_number(void)
{
    FILE *f = fopen(ASSETS_MOUNT "/build.txt", "r");
    if (!f) {
        return -1;
    }

    char line[32] = {0};
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got) {
        return -1;
    }
    return atoi(line);
}

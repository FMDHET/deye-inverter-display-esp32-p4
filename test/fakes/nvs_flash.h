#pragma once
#include "nvs.h"
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
/* Test knob: make the next nvs_flash_init() report a damaged partition. */
extern int fake_nvs_flash_init_rc;

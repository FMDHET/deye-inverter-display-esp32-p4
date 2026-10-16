idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "touch.c"
        "lvgl_port.c"
        "ui_flow.c"
        "ui_settings.c"
        "nvs_store.c"
        "assets_fs.c"
        "wifi_mgr.c"
        "captive.c"
        "web_mirror.c"
        "ota.c"
        "modbus_tcp.c"
        "modbus_rtu.c"
        "modbus_gw.c"
        "deye_ctrl.c"
        "deye_web.c"
        "meter_web.c"
        "mqtt_fwd.c"
        "ntp_client.c"
        "wg_client.c"
        "fonts.c"
        "applog.c"
        "webauth.c"
        "config_web.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web_mirror.html" "recovery.html" "deye.html"
    EMBED_FILES "montserrat_medium.ttf"
    PRIV_REQUIRES nvs_flash espcoredump mbedtls json esp_wifi esp_event esp_netif spiffs esp_http_server lwip esp_driver_jpeg app_update esp_driver_ledc esp_driver_uart mqtt esp_wireguard
)

# ---------------------------------------------------------------------------
# Gemeinsame Build-Nummer: Firmware, Dateisystem und UI tragen dieselbe Zahl.
#
# scripts/build_number.py zaehlt version.json bei JEDEM Bau genau einmal hoch
# und schreibt die Zahl nach main/build_info.h (Firmware, UI) und
# data/build.txt (SPIFFS-Image). Beim Start vergleicht die Firmware beide.
#
# Ein Ziel ohne Ausgabedatei laeuft bei jedem "idf.py build"/"flash" einmal.
# Es haengt VOR dieser Komponente (build_info.h muss vor dem Uebersetzen
# stehen) und VOR dem SPIFFS-Image (build.txt muss vor dem Packen stehen).
# idf.py monitor, menuconfig, fullclean und die Code-Ansicht in VS Code bauen
# nichts und zaehlen deshalb auch nicht.
# ---------------------------------------------------------------------------
idf_build_get_property(python PYTHON)
add_custom_target(deye_build_number
    COMMAND ${python} ${PROJECT_DIR}/scripts/build_number.py ${PROJECT_DIR}
    BYPRODUCTS ${CMAKE_CURRENT_LIST_DIR}/build_info.h ${PROJECT_DIR}/data/build.txt
    WORKING_DIRECTORY ${PROJECT_DIR}
    COMMENT "Build-Nummer hochzaehlen (version.json)"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} deye_build_number)

# SPIFFS-Partition "storage" aus data/ -> build/storage.bin. FLASH_IN_PROJECT:
# "idf.py flash" schreibt sie zusammen mit Bootloader, Tabelle und App, also
# immer im Gleichschritt. Fuer OTA wird build/storage.bin an POST /ota/fs
# geschickt. Seitengroesse, Namenslaenge und Magic kommen aus CONFIG_SPIFFS_*
# und passen damit zur Laufzeitkonfiguration in assets_fs.c.
#
# NICHT "idf.py storage-flash" allein: das baut das Image neu, zaehlt dabei die
# Nummer hoch und das Dateisystem laeuft der Firmware auf dem Geraet davon.
spiffs_create_partition_image(storage ${PROJECT_DIR}/data
    FLASH_IN_PROJECT
    DEPENDS deye_build_number)

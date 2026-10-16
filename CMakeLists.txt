# Deye-Display -- natives ESP-IDF-Projekt
# =======================================
# Hardware: GUITION JC4880P443C
#   * ESP32-P4 (+ ESP32-C6 fuer WLAN/BLE ueber ESP-Hosted)
#   * 4,3" IPS, 480x800, MIPI-DSI, GT911-Touch
#   * 32 MB PSRAM, 16 MB Flash
#
# Bauen und flashen (Firmware UND Dateisystem in einem Aufruf, damit beide
# dieselbe Build-Nummer tragen -- siehe main/CMakeLists.txt):
#     . ~/esp/v5.5.5/esp-idf/export.sh
#     idf.py -p /dev/cu.usbmodem101 flash monitor
#
# Die Konfiguration steht in "sdkconfig" (versioniert, aufgeloest) und
# "sdkconfig.defaults" (die bewusst gesetzten Werte mit Begruendung).
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF FESTGENAGELT auf 5.5.5. Mit genau dieser Version ist die Firmware
# gebaut, geflasht und geprueft worden; esp_hosted (main/idf_component.yml) und
# die C6-Firmware sind darauf abgestimmt. Frueher stand der Stift in
# platformio.ini (pioarduino 55.03.311 = IDF 5.5.5). Wer bewusst eine andere
# IDF probieren will: idf.py -DDEYE_ALLOW_ANY_IDF=1 build -- dann aber den
# ersten Build per USB mit Monitor flashen, nie blind ueber OTA.
set(DEYE_IDF_VERSION "5.5.5")
set(_idf_ver "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(NOT _idf_ver VERSION_EQUAL DEYE_IDF_VERSION AND NOT DEYE_ALLOW_ANY_IDF)
    message(FATAL_ERROR
        "Deye-Display ist auf ESP-IDF ${DEYE_IDF_VERSION} festgelegt, gefunden: "
        "${_idf_ver} (IDF_PATH=$ENV{IDF_PATH}).\n"
        "Richtige IDF aktivieren (. ~/esp/v5.5.5/esp-idf/export.sh) oder "
        "bewusst uebergehen mit -DDEYE_ALLOW_ANY_IDF=1.")
endif()

project(deye_display)

# Bauen und Flashen

## Voraussetzungen

* [PlatformIO](https://platformio.org/) (CLI oder VS-Code-Erweiterung)
* Die Plattform wird aus `platformio.ini` geladen: `pioarduino/platform-espressif32`
* Framework ist **reines ESP-IDF** (5.3 … 5.x), keine Arduino-Schicht
* Komponenten holt der IDF Component Manager beim ersten Build selbst (`main/idf_component.yml`)

Verwendete Komponenten:

| Komponente | Zweck |
| --- | --- |
| `espressif/esp_lcd_st7701` | Panel-Treiber über MIPI-DSI |
| `espressif/esp_lcd_touch_gt911` | Touch |
| `lvgl/lvgl` ^9.2 | UI-Bibliothek |
| `espressif/esp_lvgl_port` ^2.4 | getesteter LVGL-Port: Framebuffer, Rotation, tearing-freies Puffern |
| `espressif/esp_hosted` ~2.12 + `esp_wifi_remote` | WLAN über den ESP32-C6 |
| `components/esp_wireguard` (eingebunden) | WireGuard-Client |

## Der eine Befehl

```bash
pio run -e guition-p4 -t upload -t flashfs -t monitor
```

Firmware **und** Dateisystem in einem einzigen Aufruf. Das ist keine Bequemlichkeit, sondern Voraussetzung — siehe nächster Abschnitt.

Einzelne Ziele:

| Ziel | Wirkung |
| --- | --- |
| `-t upload` | Firmware über USB flashen |
| `-t flashfs` | SPIFFS-Image `storage.bin` an den Partitions-Offset schreiben |
| `-t monitor` | seriellen Monitor öffnen (115200, mit Exception-Decoder) |

## Build-Zähler

Einzige Quelle der Wahrheit ist `version.json` im Projektwurzelverzeichnis:

```json
{ "version": "1.0", "patch": 57, "patch_base": "1.0", "build": 146 }
```

`scripts/build_number.py` läuft als Pre-Build-Hook, zählt **einmal pro `pio run`** hoch und schreibt dieselbe Zahl an zwei Stellen:

1. `main/build_info.h` → wird in die Firmware und in die UI kompiliert
2. `data/build.txt` → landet im SPIFFS-Image `storage.bin`

Beim Booten liest die Firmware `/assets/build.txt` und vergleicht:

```text
I deye-display: Filesystem build matches firmware (#146)
E deye-display: BUILD MISMATCH: firmware #146 but filesystem #145 -- reflash so FW + FS + UI agree
```

Unten rechts im Hauptbildschirm steht die Version, dahinter ein **grüner Haken** bei Übereinstimmung. Über WiFi prüfbar mit `GET /ota` — `build` und `fs_build` müssen gleich sein.

Ziele, die den Zähler **nicht** hochzählen (sie erzeugen keine Firmware und dürfen das FS nicht vorlaufen lassen): `flashfs`, `uploadfs`, `buildfs`, `erase`, `monitor`, `clean`, `size`, `menuconfig` und weitere.

## Warum eigene FS-Skripte

`scripts/fs_image.py` baut das SPIFFS-Image mit demselben `spiffsgen`, das die Plattform mitbringt, und stellt ein eigenes `flashfs`-Ziel bereit, das mit `esptool` an den Partitions-Offset schreibt.

Grund: PlatformIOs eigene `buildfs`/`uploadfs`-Ziele erzwingen bei diesem reinen IDF-Projekt eine CMake-Rekonfiguration, die fehlschlägt. Und IDFs `spiffs_create_partition_image(... FLASH_IN_PROJECT)` hilft nicht, weil PlatformIO seinen eigenen Upload-Pfad fährt und IDFs `flash`-Ziel nie aufruft — das Image würde gebaut, aber nie geflasht. Deshalb steht dazu auch ein Kommentar in `CMakeLists.txt`.

Das Image wird zusätzlich nach jedem Firmware-Build automatisch erzeugt, damit `firmware.bin` und `storage.bin` immer zusammenpassen.

## Partitionslayout (16 MB)

| Name | Typ | Offset | Größe |
| --- | --- | --- | --- |
| `nvs` | data/nvs | `0x9000` | 24 KB |
| `otadata` | data/ota | `0xf000` | 8 KB |
| `phy_init` | data/phy | `0x11000` | 4 KB |
| `ota_0` | app | `0x20000` | 4 MB |
| `ota_1` | app | `0x420000` | 4 MB |
| `storage` | data/spiffs | `0x820000` | 1 MB |

Zwei App-Slots, damit sich das Gerät selbst über WiFi flashen kann. `nvs` behält seinen Standard-Offset `0x9000`, damit gespeicherte Zugangsdaten und Konfiguration eine Neupartitionierung überleben.

## Konfigurationsbesonderheiten

Auszug aus `sdkconfig.defaults` — die Einträge mit Erklärungsbedarf:

| Einstellung | Grund |
| --- | --- |
| `CONFIG_LV_USE_CLIB_MALLOC=y` | LVGLs eigener Allokator würde ein mehrere hundert KB großes statisches Array in `.bss` legen; das passt nicht in die 320 KB internes SRAM. Über newlib-`malloc` kommt PSRAM ins Spiel. |
| `CONFIG_LV_CACHE_DEF_SIZE=262144` | Tiny-TTF rastert Glyphen zur Laufzeit. Ohne Cache (Standard 0) fallen sie auf die Bitmap-Schrift zurück und Umlaute erscheinen als Kästchen. |
| `CONFIG_LV_USE_SNAPSHOT=y` | Grundlage für den Web-Mirror. |
| `CONFIG_LWIP_MAX_SOCKETS=16` | Zwei HTTP-Server, DNS, MQTT, WireGuard und persistente Modbus-TCP-Sockets brauchen mehr als die zehn Standard-Sockets. |
| `CONFIG_LWIP_TCP_MSL=5000` | Mit 60 s MSL (= 120 s `TIME_WAIT`) sammelten geschlossene HTTP-Verbindungen sich so weit an, dass der Server keine neuen mehr annahm und große OTA-Uploads abgelehnt wurden. |
| `CONFIG_ESP_NETIF_BRIDGE_EN=y` | Nur, um `LWIP_ESP_NETIF_DATA=1` zu setzen. Andernfalls stürzt der DHCP-Callback über das WireGuard-Netif ab. |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | WiFi-Init und UI-Aufbau zusammen sprengen den Standardstack. |

## Erste Installation

1. Modul per USB-C anschließen (der Port bedient UART0).
2. `pio run -e guition-p4 -t upload -t flashfs -t monitor`
3. Im Log auf `Deye-Display booting on ESP32-P4` und den Build-Vergleich achten.
4. Es erscheint der Hauptbildschirm mit `--`-Werten und der Access Point `DeyeDisplay-XXXXXX` → weiter unter [WLAN und Captive Portal](WLAN-und-Captive-Portal).
5. Danach über WiFi flashen — siehe [OTA und Recovery](OTA-und-Recovery).

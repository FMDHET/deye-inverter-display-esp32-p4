# Hardware

## Das Display-Modul

**[GUITION JC4880P443C_I_W](https://www.guition.com/esp32p4-display-module/esp32p4-display)** — ein fertig konfektioniertes Modul mit Gehäuserahmen, kein Bausatz.

| Merkmal | Wert |
| --- | --- |
| Haupt-SoC | ESP32-P4, Dual-Core RISC-V, bis 360 MHz |
| Funk-Coprozessor | ESP32-C6 auf dem Modul (WiFi 6, BLE), über SDIO angebunden |
| Panel | 4,3″ IPS TFT, 480 × 800, MIPI-DSI mit 2 Lanes @ 500 Mbps |
| Panel-Treiber | ST7701 |
| Touch | GT911, kapazitiv, I²C-Adresse `0x5D` |
| PSRAM | 32 MB (HEX-Modus, 200 MHz) |
| Flash | 16 MB |
| Weiteres | microSD-Slot, GPIO-Header |

Der ESP32-P4 hat **kein eigenes Funkmodul**. WLAN läuft vollständig auf dem C6: `esp_hosted` überträgt die WiFi-/Netzwerk-Schicht über SDIO, `esp_wifi_remote` leitet die gewohnte `esp_wifi`-API dorthin weiter. Aus Anwendungssicht sieht das wie normales `esp_wifi` aus.

> [!NOTE]
> Die Firmware des C6 muss zur Host-Version passen. Bei einem Versatz bootet das Gerät in einer Schleife neu, sobald WLAN initialisiert wird. Hier läuft C6-Slave **2.12.8** zum Host `esp_hosted ~2.12.0`.

## Panel-Anbindung

Native Ausrichtung ist Hochformat 480 × 800. Die UI läuft im Querformat 800 × 480; die Drehung um 90° macht die **PPA-Hardwareeinheit** des P4 (`CONFIG_LVGL_PORT_ENABLE_PPA=y`), nicht die CPU.

DSI-/DPI-Timings und LDO-Versorgung stehen in [`main/board_jc4880p443c.h`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/main/board_jc4880p443c.h):

| Parameter | Wert |
| --- | --- |
| Pixeltakt | 34 MHz |
| HSYNC / HBP / HFP | 12 / 42 / 42 |
| VSYNC / VBP / VFP | 2 / 8 / 166 |
| MIPI-DSI-PHY-LDO | Kanal 3, 2500 mV |
| Farbtiefe | RGB565 (16 bpp) |

Der Framebuffer belegt 480 × 800 × 2 B = 768 KB und liegt im PSRAM — deshalb ist PSRAM zwingend aktiviert.

## Pinbelegung

| Funktion | GPIO |
| --- | --- |
| LCD-Reset (`low` aktiv) | 5 |
| Hintergrundbeleuchtung (LEDC-PWM) | 23 |
| I²C SDA / SCL (Touch), 400 kHz | 7 / 8 |
| Touch-Reset | 3 |
| Touch-Interrupt | nicht verdrahtet |
| RS485 Bus A — UART1 TX / RX | 52 / 51 |
| RS485 Bus B — UART2 TX / RX | 50 / 49 |

UART0 bleibt die Konsole (115200 Baud).

## RS485

![RS485-Verdrahtung](https://raw.githubusercontent.com/FMDHET/deye-inverter-display-esp32-p4/main/docs/img/rs485.svg)

Benötigt werden **zwei Transceiver mit Auto-Direction** — Module, die die Senderichtung selbst erkennen und keinen DE/RE-Pin brauchen (z. B. verbreitete MAX485-/MAX3485-Platinen mit Automatik). Die Firmware steuert keine Richtungsleitung.

Hinweise zur Verkabelung:

* A und B durchgehend paarweise verdrillt, Abschluss 120 Ω an den Busenden.
* Gemeinsame Masse zwischen Display und Wechselrichter — sonst arbeiten die Transceiver ohne definierten Bezug.
* Beide Busse sind unabhängig: welcher Bus Master und welcher Slave ist, entscheidet die Konfiguration im Tab „Mod RTU", nicht die Verdrahtung.
* Für den Selbsttest die Busse gegeneinander verdrahten: A-TX → B-RX und A-RX → B-TX.

## Zweite Zielplatine

`sdkconfig.waveshare-p4` liegt noch im Projekt (frühere Bring-up-Plattform), wird aber nicht gepflegt und ist nicht Teil des Build-Environments. Gebaut wird ausschließlich `guition-p4`.

## Silizium-Revision

Einige ESP32-P4-Boards tragen ECO2-Silizium (Revision < 3.0). Standardmäßig baut ESP-IDF für Revision 3.x und erzeugt `xespv2p2`-Befehle, die ECO2 nicht dekodieren kann. `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` in `sdkconfig.defaults` hält das Binary zu beiden kompatibel — der Preis sind ein paar Optimierungen.

# Hardware

## Was man braucht

| Was | Warum |
| --- | --- |
| **GUITION JC4880P443C** Display-Modul | das eigentliche Gerät — Bildschirm und Computer in einem |
| **2 × RS485-Transceiver** mit Auto-Direction | damit der Chip mit dem Wechselrichter über die Zweidrahtleitung reden kann |
| USB-C-Kabel | für die erste Installation und für Fehlermeldungen |
| Zweidrahtleitung (verdrillt), 2 × 120 Ω Widerstand | die Verbindung zum Wechselrichter |

Das war es. Keine Platine löten, kein Gehäuse drucken.

## Das Display-Modul

**[GUITION JC4880P443C_I_W](https://www.guition.com/esp32p4-display-module/esp32p4-display)** ist ein fertiges Modul: Bildschirm, Touch, Rechner und Rahmen in einem Stück. Man schließt es per USB-C an und kann losprogrammieren.

| Merkmal | Wert | Was das bedeutet |
| --- | --- | --- |
| Hauptchip | ESP32-P4, 2 Kerne, bis 360 MHz | der Computer. Kein Windows, kein Linux — nur unser Programm. |
| Funkchip | ESP32-C6 | macht WLAN und Bluetooth (siehe unten) |
| Bildschirm | 4,3 Zoll, 480 × 800, IPS | knapp 11 cm Diagonale, gute Blickwinkel |
| Anschluss des Bildschirms | MIPI-DSI, 2 Leitungspaare | die schnelle Verbindung zwischen Chip und Panel |
| Bildschirmtreiber | ST7701 | der Chip direkt am Panel, der die Pixel ansteuert |
| Touch | GT911, kapazitiv | erkennt Finger wie ein Handy-Display |
| PSRAM | 32 MB | Arbeitsspeicher. Wichtig, weil das Bild allein 768 KB braucht. |
| Flash | 16 MB | der „Festplattenspeicher" für Programm und Einstellungen |
| Extras | microSD-Slot, Stiftleiste | hier hängen wir die zwei RS485-Bausteine an |

### Die Sache mit dem zweiten Chip

Der ESP32-P4 ist schnell, hat aber **kein eigenes Funkmodul**. Deshalb sitzt auf dem Modul noch ein zweiter, kleinerer Chip: der ESP32-C6. Der macht das komplette WLAN.

Die beiden reden über eine schnelle interne Verbindung (SDIO) miteinander. Damit man das beim Programmieren nicht merkt, gibt es zwei Hilfsbibliotheken: **esp_hosted** schiebt die Netzwerkdaten hin und her, **esp_wifi_remote** sorgt dafür, dass sich das im Programm anfühlt wie ein normales, eingebautes WLAN. Für uns sieht es also aus wie ein Chip — es sind aber zwei.

> [!IMPORTANT]
> Die Programme auf beiden Chips müssen zueinander passen. Passen sie nicht, startet das Gerät in einer Endlosschleife immer neu, sobald WLAN eingeschaltet wird. Hier läuft auf dem C6 die Version **2.12.8** und dazu passend `esp_hosted` in Version 2.12.x. Wenn du eine Bootschleife hast: siehe [Fehlersuche](Fehlersuche#wlan).

### Warum das Bild gedreht wird

Der Bildschirm ist im Hochformat eingebaut: 480 Pixel breit, 800 hoch. Die Anzeige soll aber im Querformat laufen — 800 breit, 480 hoch.

Diese Drehung um 90 Grad macht **nicht** der Hauptprozessor, sondern eine spezielle Hardware-Einheit im Chip namens **PPA**. Das ist wichtig, weil das Drehen von 384.000 Pixeln 30-mal pro Sekunde den Prozessor sonst stark belasten würde. Eingeschaltet wird das über die Einstellung `CONFIG_LVGL_PORT_ENABLE_PPA`.

Die genauen Zeitwerte für die Bildschirmansteuerung (wie lange ein Zeilenwechsel dauert und so weiter) stehen in [`main/board_jc4880p443c.h`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/main/board_jc4880p443c.h). Die muss man normalerweise nie anfassen — sie sind für dieses Panel erprobt. Falls das Bild verzerrt oder gestreift aussieht, sind meist genau diese Werte das Problem.

| Wert | Einstellung |
| --- | --- |
| Pixeltakt | 34 MHz |
| Zeilen-Timing (HSYNC / HBP / HFP) | 12 / 42 / 42 |
| Bild-Timing (VSYNC / VBP / VFP) | 2 / 8 / 166 |
| Spannungsversorgung des Bildschirm-Interface | LDO-Kanal 3, 2500 mV |
| Farbtiefe | RGB565, also 16 Bit pro Pixel |

## Die Anschlüsse

Welcher Anschlussstift (GPIO) welche Aufgabe hat:

| Aufgabe | GPIO |
| --- | --- |
| Bildschirm zurücksetzen | 5 |
| Hintergrundbeleuchtung (dimmbar) | 23 |
| Touch-Datenleitungen (I²C) | 7 (Daten) und 8 (Takt) |
| Touch zurücksetzen | 3 |
| **RS485 Bus A** — senden / empfangen | 52 / 51 |
| **RS485 Bus B** — senden / empfangen | 50 / 49 |

Die serielle Schnittstelle Nummer 0 bleibt frei — darüber laufen die Fehlermeldungen zum PC. Deshalb benutzen die beiden RS485-Busse die Schnittstellen 1 und 2.

## Die RS485-Verkabelung

![RS485-Verdrahtung](https://raw.githubusercontent.com/FMDHET/deye-inverter-display-esp32-p4/main/docs/img/rs485.svg)

Ein RS485-Bus ist eine sehr einfache Sache: **zwei verdrillte Adern**, genannt A und B, an denen mehrere Geräte hängen. Alle hören mit, aber nur einer darf gleichzeitig sprechen — genau wie in einer Gruppe von Leuten, die sich abwechseln müssen.

Der Mikrocontroller kann nicht direkt auf so eine Leitung sprechen; seine Signale sind zu schwach und anders geformt. Dafür gibt es die **Transceiver** — kleine Bausteine, die übersetzen. Man braucht zwei davon, für die zwei Busse.

Wichtig: nimm Module **mit „Auto-Direction"**. Bei billigen Modulen muss die Software zusätzlich eine Leitung umschalten, um zwischen Senden und Empfangen zu wechseln. Diese Firmware macht das nicht — sie erwartet Module, die das selbst erkennen.

### Fünf Regeln fürs Verkabeln

1. **A und B paarweise verdrillt** durchziehen. Nicht zwei Adern aus verschiedenen Paaren nehmen.
2. **120 Ω an beiden Enden** der Leitung zwischen A und B. Ohne diese Abschlusswiderstände wird das Signal am Leitungsende reflektiert und stört sich selbst.
3. **Gemeinsame Masse** zwischen Display und Wechselrichter. Ohne gemeinsamen Bezugspunkt „schwimmen" die Signale.
4. **A und B nicht vertauschen.** Das ist der häufigste Verdrahtungsfehler überhaupt, und man merkt ihn nur daran, dass gar nichts geht.
5. **Nicht neben Starkstromleitungen** verlegen, wenn es sich vermeiden lässt.

### Welcher Bus macht was?

Das ist **nicht** durch die Verkabelung festgelegt, sondern eine Einstellung. Jeder der beiden Busse kann entweder Fragen stellen (Master) oder Antworten geben (Slave). In der Anlage, für die das gebaut wurde, ist es so:

* **Bus A = Master** — fragt den Deye nach Ladezustand und Akkuleistung und schreibt Steuerbefehle
* **Bus B = Slave** — gibt sich als Stromzähler aus und beantwortet die Fragen des Deye

Details dazu: [Modbus-RTU](Modbus-RTU).

### Testen, ohne den Wechselrichter anzufassen

Es gibt einen eingebauten Selbsttest. Dafür verbindest du die beiden Busse einfach gegeneinander:

```text
Bus A senden  →  Bus B empfangen
Bus A empfangen  ←  Bus B senden
```

Dann fragt der eigene Master den eigenen Slave. Funktioniert das, weißt du: Transceiver, Verkabelung und Software sind in Ordnung. Wenn es danach mit dem Wechselrichter trotzdem nicht geht, liegt es an der Strecke dorthin oder an seinen Einstellungen. Zu finden im Menü unter „Mod RTU".

## Kleinere Besonderheiten

**Zwei Silizium-Generationen.** Manche ESP32-P4-Module haben einen etwas älteren Chip (genannt ECO2). Der versteht ein paar Maschinenbefehle nicht, die der Compiler standardmäßig benutzt. Damit die Firmware auf beiden Varianten läuft, steht in den Einstellungen `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. Kostet ein wenig Geschwindigkeit, verhindert aber, dass das Gerät bei manchen Leuten gar nicht startet.

**Eine zweite Platine im Projekt.** Es liegt noch eine Konfigurationsdatei für ein Waveshare-Board herum (`sdkconfig.waveshare-p4`) — das war die erste Testplattform. Sie wird nicht mehr gepflegt. Gebaut wird nur die Variante `guition-p4`.

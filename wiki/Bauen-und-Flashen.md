# Bauen und Flashen

„Bauen" heißt: aus dem Quellcode ein Programm machen. „Flashen" heißt: dieses Programm in den Speicher des Chips schreiben. Beides macht ein einziger Befehl.

## Was du installieren musst

**[PlatformIO](https://platformio.org/)** — entweder als Erweiterung in Visual Studio Code (bequemer) oder als Kommandozeilenwerkzeug. PlatformIO holt sich alles Weitere von selbst: den Compiler, die Bibliotheken, die Werkzeuge zum Flashen. Beim ersten Bauen dauert das ein paar Minuten, danach geht es schnell.

Was dabei automatisch geladen wird:

| Bibliothek | Wofür |
| --- | --- |
| `esp_lcd_st7701` | steuert den Bildschirm an |
| `esp_lcd_touch_gt911` | liest den Touchscreen aus |
| `lvgl` (Version 9) | malt Knöpfe, Kreise und Text |
| `esp_lvgl_port` | verbindet LVGL mit diesem Chip, inklusive der Bilddrehung |
| `esp_hosted` + `esp_wifi_remote` | WLAN über den zweiten Chip |
| `esp_wireguard` | der VPN-Tunnel (liegt im Projekt selbst) |

Das Projekt benutzt **kein Arduino**, sondern direkt das ESP-IDF von Espressif. Das ist etwas sperriger zu lesen, gibt aber Zugriff auf Dinge, die es unter Arduino nicht gibt — zum Beispiel den Hardware-JPEG-Encoder.

## Der eine Befehl

```bash
pio run -e guition-p4 -t upload -t flashfs -t monitor
```

Was die Teile bedeuten:

| Teil | Bedeutung |
| --- | --- |
| `pio run` | baue das Programm |
| `-e guition-p4` | für dieses Gerät (es gibt noch eine alte zweite Variante) |
| `-t upload` | schreibe das Programm über USB auf den Chip |
| `-t flashfs` | schreibe auch das kleine Dateisystem dazu |
| `-t monitor` | zeige danach die Meldungen des Geräts an |

**Wichtig: bitte immer alles in einem Befehl.** Warum, steht im nächsten Abschnitt.

Wenn es läuft, siehst du im Monitor so etwas:

```text
I deye-display: Deye-Display booting on ESP32-P4
I deye-display: Firmware build v1.0.57  (#146, ...)
I deye-display: Filesystem build matches firmware (#146)
```

Zum Beenden des Monitors: `Strg + C`.

## Warum die Build-Nummer so ein Thema ist

Es gibt hier ein Problem, das viele Bastelprojekte haben: man ändert etwas, flasht nur einen Teil und wundert sich dann, warum sich nichts ändert. Oder schlimmer: man weiß gar nicht mehr, welche Version eigentlich auf dem Gerät läuft.

Die Lösung hier ist eine **einzige Zahl für alles**. In der Datei `version.json` steht sie:

```json
{ "version": "1.0", "patch": 57, "patch_base": "1.0", "build": 146 }
```

Bei jedem Bauen zählt ein kleines Hilfsskript (`scripts/build_number.py`) diese Zahl **genau einmal** hoch und schreibt sie an zwei Stellen:

1. in eine Datei, die ins **Programm** kompiliert wird
2. in eine Datei, die ins **Dateisystem** wandert

Beim Start liest das Gerät die Zahl aus dem Dateisystem und vergleicht sie mit der Zahl in seinem Programm. Sind sie gleich, passt alles zusammen. Unten rechts auf dem Bildschirm steht dann die Version mit einem **grünen Häkchen** dahinter.

Sind sie ungleich, meldet es sich deutlich:

```text
E deye-display: BUILD MISMATCH: firmware #146 but filesystem #145
                -- reflash so FW + FS + UI agree
```

Das heißt: du hast Programm und Dateisystem getrennt geflasht, und jetzt sind sie aus dem Takt. Lösung: einfach beides nochmal in einem Befehl.

Über das Netzwerk kann man das auch prüfen:

```bash
curl -s http://<ip-des-geräts>/ota
```

Dort müssen `build` und `fs_build` dieselbe Zahl zeigen.

Übrigens: Befehle, die gar kein neues Programm erzeugen (nur Dateisystem schreiben, nur Monitor öffnen, aufräumen), zählen die Zahl **nicht** hoch. Sonst würde das Dateisystem dem Programm davonlaufen.

## Warum es eigene Skripte für das Dateisystem gibt

Kurz: weil die eingebauten Wege in diesem Fall nicht funktionieren.

PlatformIO hat eigene Befehle, um ein Dateisystem zu bauen und zu flashen. Bei einem reinen ESP-IDF-Projekt wie diesem versuchen sie dabei aber, das Build-System neu zu konfigurieren, und scheitern.

ESP-IDF hat auch einen eigenen Weg. Der baut das Dateisystem, überlässt das Flashen aber seinem eigenen Befehl — und den ruft PlatformIO nie auf. Ergebnis: das Dateisystem wird gebaut und dann nie geschrieben.

Deshalb macht `scripts/fs_image.py` das selbst: es benutzt genau dasselbe Werkzeug, das PlatformIO auch mitbringt, und bietet einen eigenen Befehl `flashfs`, der die Datei an die richtige Stelle im Speicher schreibt. Und weil es leicht zu vergessen ist, wird das Dateisystem zusätzlich nach jedem Programm-Build automatisch mit erzeugt.

## Wie der Speicher aufgeteilt ist

Der 16-MB-Flash-Speicher ist in Abschnitte („Partitionen") aufgeteilt:

| Name | Größe | Inhalt |
| --- | --- | --- |
| `nvs` | 24 KB | alle Einstellungen — WLAN-Passwörter, Geräteliste, Sollwerte |
| `otadata` | 8 KB | Merkzettel, welche Programmversion gestartet werden soll |
| `phy_init` | 4 KB | Funk-Kalibrierdaten |
| `ota_0` | 4 MB | Programmversion A |
| `ota_1` | 4 MB | Programmversion B |
| `storage` | 1 MB | das kleine Dateisystem mit der Build-Nummer |

**Warum zwei Programm-Abschnitte?** Damit sich das Gerät selbst über WLAN aktualisieren kann. Es schreibt die neue Version immer in den *gerade nicht benutzten* Abschnitt. Erst wenn die vollständig und heil angekommen ist, wird umgeschaltet. Geht beim Übertragen etwas schief, läuft die alte Version einfach weiter. Mehr dazu: [OTA und Recovery](OTA-und-Recovery).

Der Einstellungs-Abschnitt `nvs` liegt bewusst an der Standardstelle. Dadurch überleben gespeicherte WLAN-Passwörter und Konfiguration ein Update.

## Ein paar Einstellungen, die Erklärung brauchen

In `sdkconfig.defaults` stehen Optionen, die nicht offensichtlich sind. Jede davon steht dort, weil ohne sie etwas kaputt war:

| Einstellung | Warum sie da ist |
| --- | --- |
| `CONFIG_LV_USE_CLIB_MALLOC=y` | LVGL würde sonst einen mehrere hundert Kilobyte großen Speicherblock fest reservieren. Der passt nicht in die 320 KB schnellen Speicher des Chips. So benutzt LVGL den normalen Weg und landet automatisch im großen PSRAM. |
| `CONFIG_LV_CACHE_DEF_SIZE=262144` | Die Schriftarten werden zur Laufzeit aus einer Schriftdatei berechnet. Ohne Zwischenspeicher (Standard ist 0!) scheitert das lautlos und **Umlaute erscheinen als Kästchen**. |
| `CONFIG_LV_USE_SNAPSHOT=y` | Grundlage dafür, das Bildschirmbild abzugreifen — nötig für den [Web-Mirror](Web-Mirror). |
| `CONFIG_LWIP_MAX_SOCKETS=16` | Zwei Webserver, DNS, MQTT, VPN und die Modbus-Verbindungen brauchen zusammen mehr Netzwerkkanäle als die zehn Standardkanäle. **Höher darf der Wert nicht.** Mit 24 fehlte dem WLAN-Baustein beim Start der Speicher, den er für seine Übertragung braucht: Neustartschleife noch vor dem WLAN — und damit auch vor der Update-Funktion, rettbar nur per USB-Kabel. Die Modbus-Brücke bleibt deshalb bewusst sparsam: aus im Auslieferungszustand, und höchstens zwei gleichzeitige Verbindungen. |
| `CONFIG_LWIP_TCP_MSL=5000` | Beendete Netzwerkverbindungen werden normalerweise zwei Minuten lang „nachgehalten". Mit vielen kurzen Verbindungen war der Kanalvorrat dadurch erschöpft, der Webserver nahm nichts mehr an und große Updates wurden abgelehnt. Jetzt sind es 10 Sekunden. |
| `CONFIG_ESP_NETIF_BRIDGE_EN=y` | Klingt nach Netzwerkbrücke, ist aber nur ein Trick: die Option schaltet nebenbei eine andere Einstellung um, ohne die das Gerät beim VPN-Aufbau abstürzt. |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | Beim Start passiert viel gleichzeitig (WLAN hochfahren, Oberfläche bauen). Mit dem Standardwert reicht der Arbeitsplatz dafür nicht. |

## Die erste Installation, Schritt für Schritt

1. Modul mit dem USB-C-Kabel an den Rechner.
2. `pio run -e guition-p4 -t upload -t flashfs -t monitor`
3. Im Monitor prüfen: Startmeldung da? Build-Nummern gleich?
4. Auf dem Bildschirm erscheint das Energiebild — alle Werte auf `--`, weil noch keine Geräte eingerichtet sind. Das ist richtig so.
5. Das Gerät spannt ein WLAN namens `DeyeDisplay-XXXXXX` auf. Damit weiter zu [WLAN und Captive Portal](WLAN-und-Captive-Portal).
6. Ab jetzt brauchst du kein Kabel mehr — Updates gehen über WLAN, siehe [OTA und Recovery](OTA-und-Recovery).

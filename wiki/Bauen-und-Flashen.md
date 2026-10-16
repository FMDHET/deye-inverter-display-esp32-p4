# Bauen und Flashen

„Bauen" heißt: aus dem Quellcode ein Programm machen. „Flashen" heißt: dieses Programm in den Speicher des Chips schreiben. Beides macht ein einziger Befehl.

## Was du installieren musst

**[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/get-started/) in Version 5.5.5** — genau diese, nicht „irgendeine 5er". `CMakeLists.txt` prüft das und bricht mit jeder anderen Version mit einer klaren Meldung ab (warum, steht unten unter „Was festgenagelt ist").

Zwei Wege:

* **Visual Studio Code** (bequemer): Erweiterung **ESP-IDF** von Espressif installieren — das Projekt schlägt sie beim Öffnen selbst vor. Im Einrichtungsassistenten Version **v5.5.5** wählen. Danach gibt es unten in der Statusleiste Knöpfe für Bauen, Flashen und Monitor. Die Erweiterungen „PlatformIO IDE" und „pioarduino IDE" werden nicht mehr gebraucht und können deinstalliert werden.
* **Kommandozeile:**

  ```bash
  git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/v5.5.5/esp-idf
  ~/esp/v5.5.5/esp-idf/install.sh esp32p4
  . ~/esp/v5.5.5/esp-idf/export.sh      # in jedem neuen Terminal
  ```

  Fehlen danach `cmake` oder `ninja`, holt sie `python ~/esp/v5.5.5/esp-idf/tools/idf_tools.py install cmake ninja`.

> [!NOTE]
> Auf einem Mac mit mehreren Python-Versionen sucht `export.sh` die Umgebung zu dem `python3`, das gerade im PATH vorne steht. Wurde mit einer anderen Version installiert, meldet es `ESP-IDF Python virtual environment ... not found`. Dann die richtige Umgebung vorgeben:
> `export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.11_env` (Name so, wie er in `~/.espressif/python_env` steht).

Die Bibliotheken holt der Component Manager von ESP-IDF beim ersten Bauen von selbst (ein paar Minuten, danach geht es schnell):

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
idf.py -p /dev/cu.usbmodem101 flash monitor --timestamps
```

Was die Teile bedeuten:

| Teil | Bedeutung |
| --- | --- |
| `idf.py` | das Werkzeug von ESP-IDF; baut vorher automatisch, wenn sich etwas geändert hat |
| `-p /dev/cu.usbmodem101` | an diesem USB-Anschluss (unter Windows z. B. `COM5`; ohne `-p` sucht es selbst) |
| `flash` | schreibe Bootloader, Partitionstabelle, Programm **und** Dateisystem über USB auf den Chip |
| `monitor --timestamps` | zeige danach die Meldungen des Geräts an, mit Uhrzeit |

**Wichtig: bitte immer `flash`, nicht `app-flash` oder `storage-flash` allein.** Warum, steht im nächsten Abschnitt.

Wenn es läuft, siehst du im Monitor so etwas:

```text
I deye-display: Deye-Display booting on ESP32-P4
I deye-display: Firmware build v1.0.57  (#146, ...)
I deye-display: Filesystem build matches firmware (#146)
```

Zum Beenden des Monitors: `Strg + ]`.

Nur bauen, ohne Gerät: `idf.py build`. Die Ergebnisse liegen in `build/`: `deye_display.bin` (Programm), `storage.bin` (Dateisystem), `deye_display.elf` (für die Absturzauswertung).

## Warum die Build-Nummer so ein Thema ist

Es gibt hier ein Problem, das viele Bastelprojekte haben: man ändert etwas, flasht nur einen Teil und wundert sich dann, warum sich nichts ändert. Oder schlimmer: man weiß gar nicht mehr, welche Version eigentlich auf dem Gerät läuft.

Die Lösung hier ist eine **einzige Zahl für alles**. In der Datei `version.json` steht sie:

```json
{ "version": "1.0", "patch": 57, "patch_base": "1.0", "build": 146 }
```

Bei jedem Bauen zählt ein kleines Hilfsskript (`scripts/build_number.py`, von CMake als eigenes Ziel aufgerufen) diese Zahl **genau einmal** hoch und schreibt sie an zwei Stellen:

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

Übrigens: Befehle, die nichts bauen (`monitor`, `menuconfig`, `reconfigure`, `fullclean`, die Code-Ansicht in VS Code), zählen die Zahl **nicht** hoch. **Jeder** Bau dagegen zählt — auch `idf.py storage-flash`, weil es das Dateisystem neu packt. Wer nur das Dateisystem schreibt, hat danach eine neuere Nummer darin als in der Firmware auf dem Gerät. Deshalb: immer `idf.py flash`, das schreibt beides aus demselben Bau.

## Wie das Dateisystem entsteht

`main/CMakeLists.txt` ruft die eingebaute ESP-IDF-Funktion `spiffs_create_partition_image` auf: sie packt den Ordner `data/` bei jedem Bau zu `build/storage.bin`, und `idf.py flash` schreibt die Datei gleich mit (`FLASH_IN_PROJECT`). Seitengröße, Namenslänge und Kennung kommen aus derselben Konfiguration, mit der die Firmware das Dateisystem später liest — sie können also nicht auseinanderlaufen.

Zu PlatformIO-Zeiten ging das nicht, weil PlatformIO den Flash-Befehl von ESP-IDF nie aufgerufen hat; dafür gab es das eigene Skript `scripts/fs_image.py` mit dem Befehl `flashfs`. Beide sind mit dem Umstieg weggefallen. Nachgemessen: das neue `storage.bin` ist byte-identisch mit dem, was das alte Skript aus denselben Daten gebaut hat.

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
| `coredump` | 256 KB | der letzte Absturz (Task, Programmzähler, Stapelspeicher) |

**Warum zwei Programm-Abschnitte?** Damit sich das Gerät selbst über WLAN aktualisieren kann. Es schreibt die neue Version immer in den *gerade nicht benutzten* Abschnitt. Erst wenn die vollständig und heil angekommen ist, wird umgeschaltet. Geht beim Übertragen etwas schief, läuft die alte Version einfach weiter. Mehr dazu: [OTA und Recovery](OTA-und-Recovery).

Der Einstellungs-Abschnitt `nvs` liegt bewusst an der Standardstelle. Dadurch überleben gespeicherte WLAN-Passwörter und Konfiguration ein Update.

Aus demselben Grund ist `coredump` **hinten** angehängt worden und nicht irgendwo eingefügt: alle anderen Abschnitte behalten ihre Adresse, ein bestehendes Gerät verliert also nichts. Eine geänderte Partitionstabelle kommt allerdings **nur über USB** aufs Gerät — sie ist nicht Teil eines Firmware-Updates über WLAN. Ein Display, das ausschließlich per WLAN aktualisiert wurde, hat den Abschnitt deshalb nicht und meldet in `/ota` sauber `"coredump":{"present":0}`. Einmal `idf.py -p <port> flash` über Kabel behebt das dauerhaft.

## Tests

```bash
make -C test
```

Drei Suiten, die ohne Hardware laufen und in ein paar Sekunden durch sind: der
Rechenkern des Regelpfads (`compute_served`, der komplette Slave-Pfad von der
Anfrage bis zur Antwort, CRC, Konfig-Grenzen), das Passwort-Tor der
Web-Schnittstellen und die Akku-Steuerung (Registerfolgen, Rückleseverifikation,
2-Stunden-Ablauf, Neustart-Pfad). Zusammen über 200 Prüfungen. Kein
Framework und kein Download — ein Compiler und ein Makefile, damit die Tests
laufen, bevor irgendetwas geholt werden muss.

Was dort **nicht** getestet wird, ist alles, was einen Bus, einen Bildschirm
oder ein Netz braucht; das sind Gerätetests und stehen als Messreihen im
[Code-Review](Code-Review-2026-09). Details, Aufbau und die Regel für die
Attrappen: [`test/README.md`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/test/README.md).

In [GitHub Actions](https://github.com/FMDHET/deye-inverter-display-esp32-p4/actions) laufen die Suiten bei jedem Push, dazu ein Firmware-Bau mit `idf.py build` im offiziellen ESP-IDF-v5.5.5-Container von Espressif. Weil die Version festgenagelt ist, heißt ein roter Bau: hier im Projekt ist etwas kaputt. Der Bau legt `deye_display.bin`, `deye_display.elf`, `storage.bin`, Bootloader und Partitionstabelle als Artefakt ab — das ELF ist genau das, was man zum Auswerten eines Coredumps braucht.

## Ein paar Einstellungen, die Erklärung brauchen

In `sdkconfig.defaults` stehen Optionen, die nicht offensichtlich sind. Jede davon steht dort, weil ohne sie etwas kaputt war:

| Einstellung | Warum sie da ist |
| --- | --- |
| `CONFIG_LV_USE_CLIB_MALLOC=y` | LVGL würde sonst einen mehrere hundert Kilobyte großen Speicherblock fest reservieren. Der passt nicht in die 320 KB schnellen Speicher des Chips. So benutzt LVGL den normalen Weg und landet automatisch im großen PSRAM. |
| `CONFIG_LV_CACHE_DEF_SIZE=262144` | Die Schriftarten werden zur Laufzeit aus einer Schriftdatei berechnet. Ohne Zwischenspeicher (Standard ist 0!) scheitert das lautlos und **Umlaute erscheinen als Kästchen**. |
| `CONFIG_LV_USE_SNAPSHOT=y` | Grundlage dafür, das Bildschirmbild abzugreifen — nötig für den [Web-Mirror](Web-Mirror). |
| `CONFIG_LWIP_MAX_SOCKETS=16` | Zwei Webserver, DNS, MQTT, VPN und die Modbus-Verbindungen brauchen zusammen mehr Netzwerkkanäle als die zehn Standardkanäle. 16 ist der Wert, mit dem dieses Gerät erprobt läuft; 24 wurde probiert, ist aber nie zum Laufen gekommen (die Ursache lag woanders, siehe unten). Die Modbus-Brücke ist deshalb so bemessen, dass sie in dieses Budget passt: aus im Auslieferungszustand, höchstens zwei gleichzeitige Verbindungen. |
| `CONFIG_LWIP_TCP_MSL=5000` | Beendete Netzwerkverbindungen werden normalerweise zwei Minuten lang „nachgehalten". Mit vielen kurzen Verbindungen war der Kanalvorrat dadurch erschöpft, der Webserver nahm nichts mehr an und große Updates wurden abgelehnt. Jetzt sind es 10 Sekunden. |
| `CONFIG_ESP_NETIF_BRIDGE_EN=y` | Klingt nach Netzwerkbrücke, ist aber nur ein Trick: die Option schaltet nebenbei eine andere Einstellung um, ohne die das Gerät beim VPN-Aufbau abstürzt. |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | Beim Start passiert viel gleichzeitig (WLAN hochfahren, Oberfläche bauen). Mit dem Standardwert reicht der Arbeitsplatz dafür nicht. |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | Der Wachhund, der eine nicht startende Firmware von selbst zurücknimmt. Siehe [OTA und Recovery](OTA-und-Recovery#der-automatische-rückfall). Wirkt erst, wenn der Bootloader einmal per USB geschrieben wurde. |
| `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` | Übersetzen auf Größe (`-Os`) statt auf Debugbarkeit (`-Og`). `-Og` war die Vorgabe, keine Entscheidung: dieses Gerät wird nicht per JTAG durchgesteppt, und ein Absturz wird über den Coredump ausgewertet — dafür braucht es Symbole im ELF, keine Optimierungsstufe. Gemessen: 2 148 784 → 1 936 720 Byte, rund 10 % weniger, und der freie DMA-Speicher stieg von 54 auf 71 kB — genau der Vorrat, an dem die OTA-Abstürze hingen. Die **Zusicherungen bleiben an**; sie abzuschalten wäre die naheliegende zweite Hälfte eines „Release-Builds" und genau die falsche. |
| `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` | Ein Absturz hinterlässt Task, Programmzähler und Stapelspeicher im Flash statt nur das Wort „PANIC". Siehe [Fehlersuche](Fehlersuche#nach-einem-absturz). |

## Was festgenagelt ist, und warum genau so

Ein Build, der sich beim ersten Aufruf holt, was gerade neu ist, ist kein
Build, den man wiederholen kann. Hier ist deshalb **jede** bewegliche Stelle
festgelegt — aber jede an ihrer eigenen, richtigen Stelle.

**ESP-IDF selbst** (`CMakeLists.txt`):

```cmake
set(DEYE_IDF_VERSION "5.5.5")
```

Mit jeder anderen Version bricht der Bau gleich am Anfang ab und sagt, welche er gefunden hat. Zwei Checkouts derselben Firmware können damit nicht mehr mit verschiedenen ESP-IDF-Versionen gebaut werden. Die CI nagelt dieselbe Version ein zweites Mal fest (`esp_idf_version: v5.5.5` in `.github/workflows/ci.yml`). Wer bewusst eine andere Version ausprobieren will, schaltet die Prüfung mit `idf.py -DDEYE_ALLOW_ANY_IDF=1 build` ab — und flasht diesen Bau dann per USB mit angehängtem Monitor, nie blind über WLAN.

Früher stand diese Festlegung in `platformio.ini` als Plattform-Marke `55.03.311` von pioarduino, die ESP-IDF 5.5.5 mitbrachte. Das Projekt war schon immer reines ESP-IDF; PlatformIO war nur die Hülle darum. Seit Oktober 2026 wird direkt mit `idf.py` gebaut. Die Konfiguration ist dabei unverändert geblieben: `sdkconfig` (früher `sdkconfig.guition-p4`) enthält dieselben Werte, `dependencies.lock` kommt byte-identisch heraus.

**Der Startabsturz** ist an seiner tatsächlichen Ursache festgelegt, nicht an
der IDF-Version:

```text
assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)
```

Das passiert in der Startroutine des WLAN-Bausteins — **bevor** das Programm
anläuft. Kein Bildschirm, kein WLAN, keine Update-Funktion; Rettung nur per
Kabel. Schuld war nicht die IDF, sondern `esp_hosted`, das von `~2.12.0` auf
2.12.12 hochgelaufen war. In `main/idf_component.yml` steht deshalb
`espressif/esp_hosted: "==2.12.8"` — die Version, die zur C6-Firmware auf
diesem Modul passt. Host und Ko-Prozessor sind hier ein Paar: anheben nur
zusammen, und den ersten Build per USB mit angehängtem Monitor flashen, nie
blind über WLAN.

**Die übrigen Komponenten** stehen als Bereiche im Manifest (`^9.2.0`, `*`) —
festgehalten werden sie durch **`dependencies.lock`**, und die Datei ist
absichtlich eingecheckt (früher war sie ignoriert). Dass sie wirklich bindet,
ist nachgemessen: `managed_components` wegwerfen, neu bauen — die Lock-Datei
kommt byteidentisch zurück, mit LVGL 9.5.0, esp_lvgl_port 2.9.0, esp_hosted
2.12.8. Ein neu veröffentlichtes 9.6 ändert daran nichts, solange das Manifest
gleich bleibt. Die CI prüft genau das mit `git diff --exit-code --
dependencies.lock`: schreibt ein Bau die Datei um, sind Manifest und Sperre
auseinandergelaufen und jemand muss hinsehen.

**Anheben** heißt also: neue ESP-IDF-Version installieren, `DEYE_IDF_VERSION`
in `CMakeLists.txt` und `esp_idf_version` in der CI gemeinsam tauschen, einmal
komplett neu bauen (`idf.py fullclean build`), per USB aufs Gerät flashen,
prüfen — und die geänderte `sdkconfig` und `dependencies.lock` mit committen.

## Die erste Installation, Schritt für Schritt

1. Modul mit dem USB-C-Kabel an den Rechner.
2. `idf.py -p /dev/cu.usbmodem101 flash monitor --timestamps` (Anschluss anpassen)
3. Im Monitor prüfen: Startmeldung da? Build-Nummern gleich?
4. Auf dem Bildschirm erscheint das Energiebild — alle Werte auf `--`, weil noch keine Geräte eingerichtet sind. Das ist richtig so.
5. Das Gerät spannt ein WLAN namens `DeyeDisplay-XXXXXX` auf. Damit weiter zu [WLAN und Captive Portal](WLAN-und-Captive-Portal).
6. Ab jetzt brauchst du kein Kabel mehr — Updates gehen über WLAN, siehe [OTA und Recovery](OTA-und-Recovery).

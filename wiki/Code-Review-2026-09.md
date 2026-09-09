# Code-Review September 2026

Vollständige Durchsicht der Firmware (rund 13 400 Zeilen C, HTML und Python) am 8. September 2026, aufgeteilt auf fünf Teilbereiche: OTA und Webserver, Modbus-Regelpfad, Kern (Boot, WLAN, NVS, MQTT, NTP, WireGuard), LVGL-Oberfläche und die eingebetteten Webseiten. Jeder Bereich wurde Zeile für Zeile gelesen; die Funde wurden am angeschlossenen Gerät nachgestellt, wo das möglich war.

Diese Seite hält fest, **was behoben wurde** (mit Nachweis), **was offen ist** (nach Dringlichkeit) und **was gut ist und so bleiben soll**. Die offenen Punkte im Regelpfad sind bewusst nicht „nebenbei" geändert worden: sie betreffen das Verhalten gegenüber dem Wechselrichter und brauchen eine Entscheidung des Betreibers.

## Zusammenfassung

* **OTA über WLAN ist repariert.** Ursache war der WLAN-Treiber esp_hosted, der seine SDIO-Empfangspuffer aus dem knappen internen DMA-Speicher holte und bei Knappheit abstürzte statt einen Fehler zu melden. Mit `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` kommen die Puffer aus dem PSRAM. Messreihe: 10 Firmware- und 3 Dateisystem-Updates hintereinander, null Abbrüche (vorher etwa jeder vierte).
* **Sieben weitere Fehler im OTA-Pfad behoben**, darunter zwei, die das Gerät lahmlegen konnten: ein Upload, der mitten drin stehen bleibt, fror das Gerät dauerhaft ein; eine falsche Datei im Firmware-Feld löschte den einzigen Rückfall-Abschnitt.
* **Die Web-Oberfläche `/deye` ist jetzt auf dem iPhone benutzbar**: Tabellenzeilen werden zu Karten, die eingegebenen Werte werden vor dem Senden geprüft, das Polling stapelt sich nicht mehr.
* **Der Modbus-Regelpfad ist entschieden und umgesetzt** (Nachtrag 4): ein ausgefallenes Gerät blockiert seine IP-Mitbewohner nicht mehr, der CT-Eingang des Deye ist kein Regelwert mehr (Regel- und Anzeigewert sind jetzt getrennte Variablen), ein Zwangsmodus wird beim Start abgeräumt und läuft nach 2 h ab, und das 0-W-Halten ist eine Brücke mit Ablauf statt eines Dauerzustands.
* **Offen und wichtig:** die Sicherheit des Notfall-WLANs (AP-Passwort ist die veröffentlichte Konstante, `/ota` und die Deye-Steuerung ohne Passwort) und die Versionierung der Konfig-Blobs. Die Display-Oberfläche ist abgearbeitet (Nachträge 1–3), darunter die zwei Fehler, die das Gerät unbedienbar machen konnten: Touch-Ausfall beim Start und Helligkeit 0 %.

## Behoben

### OTA und Webserver

| Was | Datei | Nachweis |
| --- | --- | --- |
| SDIO-Transportpuffer aus dem PSRAM statt aus dem internen DMA-Pool. Das war die Absturzursache: `assert failed: sdio_rx_get_buffer sdio_drv.c:953`. | `sdkconfig.defaults`, `sdkconfig.guition-p4` | `dma_min` in `/ota` von ~8 kB auf ~97 kB; 10/10 Firmware-OTA, 3/3 FS-OTA ohne Abbruch |
| Upload-Timeout begrenzt. `httpd_req_recv` meldet nach 12 s einen Timeout, und die Schleife hat das endlos wiederholt — mit dunklem Panel, gehaltener LVGL-Sperre und blockiertem Webserver. Jetzt drei Versuche, dann Abbruch und Oberfläche wieder frei. | `main/ota.c` (`ota_recv`) | Codepfad; am Gerät nicht provoziert |
| Firmware-Kopf **vor** dem Löschen prüfen: Magic, Chip-ID, Projektname. Vorher wurde der ganze 4-MB-Zielabschnitt gelöscht, bevor das erste Byte angesehen wurde — `storage.bin` im Firmware-Feld vernichtete den Rückfall-Abschnitt. | `main/ota.c` (`ota_image_check`) | `storage.bin` an `/ota` → `400 not a firmware image`, Gerät läuft weiter, Slot unberührt |
| `esp_ota_begin` mit exakter Größe statt `OTA_SIZE_UNKNOWN`: nur die nötigen Sektoren werden gelöscht, kürzere Dunkelphase. | `main/ota.c` | Upload 2,1 MB in 19 s |
| Dateisystem-OTA hängt SPIFFS aus und wieder ein. Vorher beschrieb es die Partition unter dem laufenden Dateisystem; `fs_build` zeigte bis zum Neustart den alten Stand. Jetzt sofort korrekt, kein Neustart nötig. Löschen sektorweise statt 1 MB auf einmal. | `main/ota.c`, `main/assets_fs.c` | `FS OK, build #213 mounted`, `fs_build` sofort aktuell |
| Bewährung statt Sofort-Bestätigung. Das Image wurde direkt nach dem Start des Webservers bestätigt, bevor irgendetwas gelaufen war — eine Firmware, die zwei Sekunden später abstürzt, lief damit in einer Endlosschleife. Jetzt: nach 60 s bei Erreichbarkeit, durch jedes eingehende Update, spätestens nach 10 min. | `main/ota.c` (`ota_arm_confirm`), `main/main.c` | Serial: `probation over after 62s (reachable) -- confirming image` |
| RTU-Master und Deye-Steuerung starten unabhängig vom WLAN. Sie hingen im `else`-Zweig von `wifi_mgr_init` — ein C6, der beim Start hakt, nahm dem Deye den Zähler weg. | `main/main.c` | Codepfad |
| Pufferüberlauf in `/api/devices`: `o += snprintf(...)` wurde erst nach dem Schreiben geprüft; bei acht Geräten mit langen Namen rechnete `sizeof(j) - o` in den negativen Bereich und schrieb hinter den Stack-Puffer. | `main/web_mirror.c` | Codepfad |
| Task-Watchdog löst jetzt einen Neustart aus (`CONFIG_ESP_TASK_WDT_PANIC=y`) statt alle 5 s eine Logzeile zu schreiben. tcpip-Task-Stack 3072 → 6144 B, weil WireGuard seine Kryptografie in diesem Thread rechnet (die Komponente verlangt 5 kB). | `sdkconfig.defaults`, `sdkconfig.guition-p4` | Build sauber, 10 Neustarts |
| `GET /ota` liefert `Cache-Control: no-store` und die DMA-Kennzahlen `dma`, `dma_max`, `dma_min`. Während eines Updates protokolliert die Firmware alle 512 kB den DMA-Stand. | `main/ota.c` | `curl -si` |

### Web-Oberfläche

| Was | Datei |
| --- | --- |
| **Responsive für das iPhone (≤ 640 px):** die vier Tabellen des Zähler-Tabs (Phasenübersicht, Manipulation, Geräteliste, Deye je Phase) werden zu Karten — jede Zeile ein Block, die erste Zelle der Titel, die übrigen ein Raster aus Label und Wert. Die Labels setzt das JavaScript als `data-l`; das CSS zeigt sie per `::before`. Am Desktop bleibt alles wie es war. Tabs teilen sich die Breite, statt bei 375 px umzubrechen; Tippziele mindestens 40–44 px; `input[type=file]` 16 px (kein iOS-Zoom). | `main/deye.html` |
| **Leere Felder gehen nicht mehr als 0 W an den Wechselrichter.** `parseInt("") \|\| 0` machte aus einem gelöschten Sollwert einen Netz-Sollwert von 0 W. Jetzt Prüfung vor dem Senden, Meldung am Feld. Werte auf eine Nachkommastelle gerundet (mehr speichert die Firmware nicht). | `main/deye.html` |
| **Polling stapelt sich nicht mehr.** `setInterval` startete jede Sekunde eine neue Anfrage, egal ob die vorige fertig war — bei einem langsamen Gerät gehörten nach wenigen Sekunden alle vier Sockets dieser Seite. Jetzt eine selbst nachziehende Schleife mit 4-s-Timeout, im Hintergrund-Tab pausiert. | `main/deye.html` |
| **XSS über Gerätenamen.** Name, IP, Typ und Rolle kamen JSON-, aber nicht HTML-escaped in `innerHTML`. Ein Gerätename `<svg onload=…>` (21 Zeichen, passt in 24) hätte im Browser des Admins Skript ausgeführt — und diese Seite darf Register schreiben. Jetzt `esc()`. | `main/deye.html` |
| Register-Schreibbefehle mit `cache:'no-store'`: Safari konnte einen wiederholten identischen Schreibbefehl aus dem Cache beantworten — „OK" ohne dass etwas auf den Bus ging. | `main/deye.html` |
| **Update-Ablauf im System-Tab und auf der Recovery-Seite:** HTTP-Fehler werden als Fehler gezeigt (vorher stand ein 500er-Text als Erfolg da), Buttons sind während des Uploads gesperrt, nach dem Neustart wird `/ota` gepollt, bis das Gerät antwortet — und dann gesagt, ob der Build gewechselt hat oder der Rollback gegriffen hat. Vorher meldete ein fester 4-s-Timer nach *jedem* erfolgreichen Update „nicht erreichbar". Dateinamen, die nicht zum Ziel passen, werden nachgefragt. Recovery-Seite zeigt jetzt auch `fs_build` und den Neustartgrund. | `main/deye.html`, `main/recovery.html` |
| Statusfarben aus den CSS-Variablen statt fester Dunkel-Palette (`#3aa675` auf Weiß hatte 2,9:1). | `main/deye.html` |
| Display-Spiegel: `pointermove` gedrosselt (iOS liefert 60+ pro Sekunde, jedes war ein Request); `100dvh` statt `100vh`, damit das Bild nicht hinter Safaris Leisten verschwindet. | `main/web_mirror.html` |

## Offen — nach Dringlichkeit

### Modbus-Regelpfad — entschieden und umgesetzt (Nachtrag 4)

Diese vier Punkte betreffen, was der Wechselrichter zu sehen bekommt, und wurden deshalb nicht „nebenbei" geändert, sondern dem Betreiber vorgelegt. Am 9. September hat er alle vier entschieden; die Umsetzung steht in [Nachtrag 4](#nachtrag-4-9-september-der-modbus-regelpfad). Die Funde im Original:

1. **Gemeinsame IP: ein ausgefallenes Gerät blockiert den Netzzähler.** `modbus_tcp.c` ~721–759: Beim ersten Fehler in der Geräteliste einer IP schließt der Worker den Socket, bricht die Runde ab und schläft 3 s. Fronius-Aufbau mit Smart Meter (Unit 240) und Wechselrichter (Unit 1) auf *derselben* IP: nachts schaltet der Symo ab, die Reads an Unit 1 laufen in den Timeout, der Zähler wird nie mehr gelesen, nach 12 s gilt er als veraltet, die Emulation sendet 0 W — die ganze Nacht. Vorschlag: nur bei Transportfehlern abbrechen, bei Modbus-Exceptions zum nächsten Gerät weitergehen; Netzzähler-Rollen zuerst abfragen.
2. **Deye-CT als „frischer" Netzwert = Rückkopplung.** `modbus_tcp.c` ~746: Ohne Netzzähler-Rolle wird der CT-Eingang des Deye (Register 619) als gültiger Netzwert übernommen. Das ist aber genau der Wert, den unsere Emulation ihm zuletzt geschickt hat — die Schleife füttert sich selbst. Vorschlag: `s_grid_valid` nie aus `deye_ct` setzen; Anzeige ja, Regelung nein.
3. **Erzwungener Akku-Modus überlebt keinen Neustart des Displays — der Wechselrichter behält ihn aber.** `deye_ctrl.c`: Modus nur im RAM, Rückgabewerte der zwölf Schreibbefehle werden verworfen, nichts wird zurückgelesen. Nach einem OTA sagt das Display „Normal", der Deye lädt weiter mit 5 kW aus dem Netz. Vorschlag: Modus mit Zeitstempel in NVS, beim Start zurücksetzen oder wiederherstellen, Maximaldauer mit automatischem Rückfall, Schreibbefehle verifizieren.
4. **0-W-Halten bei veraltetem Zähler ist zeitlich unbegrenzt.** `modbus_rtu.c` ~292 und ~325 (zwei sich widersprechende Kommentare). Zehn Minuten Router-Neustart bei 5 kW Entladung: der Deye hält 5 kW, egal wie sich die Last ändert. Ein *stummer* Zähler würde stattdessen die Zählerausfall-Behandlung des Deye auslösen. Vorschlag: 0 W nur als kurze Überbrückung (30–60 s), danach nicht mehr antworten; am Gerät prüfen, was der Deye bei Zählerausfall tut.

Entschieden wurde: (1) weitergehen statt Runde abbrechen, dazu eine Altersgrenze je Gerät; (2) Regelung sperren, Anzeige behalten; (3) beim Start auf Normal zurücksetzen, mit Maximaldauer und geprüften Schreibbefehlen; (4) 0 W nur 60 s als Überbrückung, danach stumm.

Weitere Funde mittlerer Schwere: Netz-Sollwert ohne Grenzen im Modul (`modbus_rtu_set_grid_setpoint`, auch beim Laden aus NVS); `deye_req_run` kann nach einem Timeout das Ergebnis der *vorigen* Anfrage an den nächsten Aufrufer liefern (die Schwester-Funktion `modbus_rtu_txn` macht es richtig); keine Absicherung gegen verspätete RS485-Antworten, FC16-Echo wird nicht mit der Anfrage verglichen; Modbus-TCP-Transaktions-ID ist konstant `1`; SLS-Exportschutz rechnet mit veralteten Daten und schreibt bei jeder ±200-W-Änderung in EEPROM-Register; Phasenmanipulation bleibt über Neustarts aktiv, ohne Ablauf und ohne Hinweis auf dem Hauptbildschirm; `poll_ms` bis 60 s erlaubt, obwohl der Zähler nach 12 s als veraltet gilt; Selbsttest sendet auf Bus 1 unabhängig von dessen Rolle; die Bridge erlaubt jedem im LAN Schreibzugriff auf alle Deye-Register.

### Kern und Sicherheit

* ~~Notfall-WLAN wird nie wieder abgebaut~~ — **behoben (Nachtrag, siehe unten).** Offen bleibt: das AP-Passwort ist weiterhin die veröffentlichte Konstante (`nvs_store_set_ap_psk` hat keinen Aufrufer), und `/ota` sowie die Deye-Steuerung haben kein Passwort.
* ~~Netzwerkwahl springt zum falschen Netz~~ — **behoben (Nachtrag).**
* ~~WireGuard wird genau einmal versucht~~ — **behoben (Nachtrag).**
* `mqtt_apply()` läuft auf dem LVGL-Task und zerstört den Client, während der MQTT-Task ihn benutzen kann; MQTT-Kommandos: unbekannter Modus wird als Normal *angewendet*, `atoi("abc")` = 0 W wird auf 1000 W geklemmt und angewendet, kein Schalter „Steuerung per MQTT erlauben".
* Konfigurations-Blobs `mqtt`/`ntp`/`wg` ohne Versionsfeld: ein Feld anhängen, OTA, Rollback → die ältere Firmware verwirft die Einstellungen stillschweigend.
* Reproduzierbarkeit: `platform` folgt dem Git-HEAD, `dependencies.lock` ist gitignored, `sdkconfig.guition-p4` ist eingecheckt und schlägt `sdkconfig.defaults` — Änderungen dort wirken auf bestehenden Checkouts nicht. `CONFIG_COMPILER_OPTIMIZATION_DEBUG` (`-Og`) im Produktivbetrieb.
* Zwei Überläufe in `captive.c` (`h_scan`, `h_connect`) sind heute unerreichbar, weil `captive_portal.html` gar nicht mehr eingebettet wird — tote Seite plus tote Handler, 4 kB statischer RAM.

### Display-Oberfläche (LVGL)

* ~~Touch-Ausfall beim Start = Endlosschleife~~ — **behoben (Nachtrag).**
* ~~Helligkeit 0 % wird gespeichert~~ — **behoben (Nachtrag).**
* ~~Der Aufweck-Tipp wird an das Widget darunter durchgereicht~~ — **behoben (Nachtrag 3).**
* ~~Vier Speicher-Callbacks schreiben Default-Literale als Nutzerwahl in NVS~~ — **behoben (Nachtrag 3).**
* ~~VPN-Tastatur schwebt über andere Tabs~~, ~~jeder RTU-Dropdown-Tick schreibt synchron in NVS und blendet die Deye-Anzeige aus~~, ~~Scan-Liste kann bei „scanne…" hängen~~, ~~Deye-Leistungsslider 0–22000 gegen Backend 1000–20000~~, ~~kein `max_length` auf Textfeldern~~ — **alle behoben (Nachträge 2 und 3).**

### OTA und Web, klein

* `/api/devices` escaped Gerätenamen nicht (JSON bricht bei `"`); `jesc` aus `meter_web.c` sollte geteilt werden.
* `scripts/build_number.py` zählt bei IDE-Targets (`idedata`, IntelliSense) und bei fehlgeschlagenen Builds hoch — deshalb ist `version.json` ständig geändert.
* Kein Coredump-Abschnitt (6,9 MB Flash frei), `TASK_WDT` konnte bisher nie als Neustartgrund erscheinen.
* `/ota/rollback` prüft den Zustand des anderen Slots nicht (`ABORTED`/`INVALID` → Neustart ohne Wirkung).
* DNS-Hijack im Captive Portal hängt die Antwort hinter einen mitkopierten EDNS-OPT-Record; Clients mit EDNS0 (Windows, Chrome) sehen eine kaputte Antwort.
* Register-Tab: `probeRead` ohne Wiedereintrittsschutz; `dirty`-Flag im Zähler-Tab wird nur durch Senden gelöscht; Spaltengriffe ohne `pointercancel`; Theme-Markierung ignoriert `?theme=`.

## Nachtrag (gleicher Tag): fünf weitere Punkte behoben

| Was | Datei | Wie |
| --- | --- | --- |
| Touch-Ausfall beim Start | `main/touch.c`, `main/main.c`, `main/lvgl_port.c` | GT911 wird an 0x5D und dann 0x14 gesucht; fehlt er, läuft das Gerät ohne Touch weiter (Meldung im Log, Web-Spiegel bleibt bedienbar). Kein `ESP_ERROR_CHECK` mehr, I2C-Bus wird im Fehlerfall freigegeben. |
| Helligkeit 0 % | `main/display.c/.h`, `main/ui_settings.c` | Untergrenze 5 % in `display_set_brightness()`; der Slider beginnt dort, ein gespeicherter Wert darunter springt hoch. 0 gibt es nur noch als internes „aus" von `display_backlight(false)`. |
| Notfall-WLAN bleibt an | `main/wifi_mgr.c` | Nach `GOT_IP` startet eine 60-s-Nachfrist; danach schaltet der Worker auf reinen STA-Modus, sofern kein Client mehr am AP hängt (sonst in einer Minute noch einmal). Der DNS-Hijack des Captive Portals folgt `ap_active` von selbst. |
| Netzwerkwahl springt weiter | `main/wifi_mgr.c` | `WIFI_REASON_ASSOC_LEAVE` — der Code, den nur unser eigenes `esp_wifi_disconnect()` erzeugt — wird nicht mehr als gescheiterter Versuch gewertet. Zustand wird vor dem Disconnect gesetzt, nicht danach. Dazu: schlägt `esp_wifi_connect()` selbst fehl, wird der langsame Retry neu gestartet statt ewig zu warten. |
| WireGuard nur ein Versuch | `main/wg_client.c` | Alle 30 s erneut, sobald STA eine IP hat; ein Tunnel ohne Handshake seit 3 min wird neu aufgebaut (Endpoint neu aufgelöst). |

Verifiziert am Gerät: Boot sauber, Zähler-Emulation läuft, OTA über WLAN (diesmal ohne USB — der Fix aus dem Hauptteil in der Praxis). Touch-Ausfall und AP-Abbau sind Codepfade, die ohne Hardware-Eingriff bzw. Router-Ausfall nicht provozierbar waren.

## Nachtrag 2: die mittleren Funde ohne Regelpfad-Entscheidung

| Bereich | Was | Wie |
| --- | --- | --- |
| MQTT | Unbekannter Modus wurde als „Normal" *angewendet*; `atoi("abc")` = 0 W wurde auf 1000 W geklemmt und angewendet | Unbekannte Payloads werden abgelehnt und der echte Zustand erneut veröffentlicht; Leistung wird streng geparst (`strtol`, Bereich 1000–20000) |
| MQTT | `mqtt_apply()` lief auf dem LVGL-Task und zerstörte den Client, während der MQTT-Task ihn nutzen konnte; `esp_mqtt_client_stop()` blockierte die Oberfläche | Neuaufbau nur noch im MQTT-Task (`s_restart`); vorher wird retained „offline" veröffentlicht, sonst zeigte HA das Gerät nach dem Abschalten ewig als verfügbar |
| MQTT | HA-Slider sprang bei jedem Eingriff des SLS-Schutzes | `deye_power` meldet den Nutzer-Sollwert, nicht den gedrosselten Wert |
| Modbus | Netz-Sollwert ohne Grenzen im Modul und beim Laden aus NVS | ±30 kW (wie das Web-Formular), auch beim Boot; ein Sollwert ≠ 0 wird beim Start protokolliert |
| Modbus | `deye_req_run` lieferte nach Timeout das Ergebnis der *vorigen* Anfrage an den nächsten Aufrufer | Bus-Task arbeitet auf einer Kopie und schreibt nur zurück, wenn die Anfrage noch wartet; nach Timeout Nachfrist wie in `modbus_rtu_txn` |
| Modbus | FC16-Echo nicht mit der Anfrage verglichen; verspätete Antworten wurden als Antwort auf die nächste Anfrage akzeptiert | Echo muss Adresse/Anzahl der Anfrage nennen; nach jedem Timeout wird die Leitung bis 50 ms Ruhe geleert; `rtu_raw` prüft den Funktionscode |
| Web | `/api/devices` brach bei `"` im Gerätenamen | Namen und IPs JSON-escaped |
| OTA | `/ota/rollback` startete auch in einen vom Bootloader abgelehnten Slot neu — ohne Wirkung | Slot-Zustand wird geprüft (`INVALID`/`ABORTED` → 400 mit Begründung); `GET /ota` liefert `running_state`, `other_state`, `other_version` |
| Build | `build_number.py` zählte bei IDE-Targets (`idedata`) und Fehl-Builds hoch — `version.json` war nie sauber | Allowlist: nur `upload`/`program`/`buildprog` und ein nacktes `pio run` zählen |
| LVGL | Scan-Liste blieb bei „scanne…", wenn ein Rescan gleich viele Netze fand | Zähler wird pro Scan zurückgesetzt |
| LVGL | Bildschirmtastatur schwebte über andere Tabs und fing weiter Tasten | Beim Tab-Wechsel werden alle Tastaturen versteckt, `s_active_ta` gelöscht |
| LVGL | Kein `max_length` auf Textfeldern (stilles Abschneiden beim Speichern) | Feldgrenzen = Strukturgrenzen; Zahlenfelder nur Ziffern, IP-Felder Ziffern und Punkt |
| LVGL | Deye-Leistungsslider 0–22000 gegen Backend 1000–20000 | `DEYE_POWER_MIN/MAX` aus `deye_ctrl.h`, eine Definition für Slider, HA-Discovery und MQTT-Parser |
| `/deye` | `dirty`-Flag nur durch Senden gelöscht — Formular folgte dem Gerät nie wieder | Stimmt das Formular mit dem Gerät überein, ist nichts mehr `dirty` |
| `/deye` | `probeRead` ohne Wiedereintrittsschutz; leere Anzahl ergab „0 Register gelesen" in Grün | Sperre während des Lesens, Adresse/Anzahl werden geprüft |
| `/deye` | Jeder Fehler der Live-Werte hieß „Firmware ohne /api/deye/live?" | Netzwerkfehler, 404 und HTTP-Fehler unterschieden; Kacheln werden gedimmt statt alte Zahlen als aktuell stehen zu lassen |

Bewusst **nicht** in diesem Nachtrag: die Klemmung des Ladestroms (`amps = power_w / 50` bis 400 A in Register 128) braucht die Grenze des Akkus/BMS als Konfiguration; die Konfig-Blob-Versionierung ändert das NVS-Layout; der Aufweck-Tipp und die Speicher-Callbacks (Defaults als Nutzerwahl) folgen im nächsten Schritt.

## Nachtrag 3 (9. September): Aufweck-Tipp, Speicher-Callbacks, RTU-Dropdowns

Damit ist der LVGL-Abschnitt abgearbeitet. Build 231 (v1.0.142), alles am Gerät nachgestellt.

| Bereich | Was | Wie |
| --- | --- | --- |
| LVGL | **Der Tipp, der das dunkle Panel aufweckt, landete zusätzlich im Widget darunter** — links liegt der Netz-Sollwert-Slider über die volle Höhe, Aufwecken konnte also den Sollwert verschieben | Solange das Panel schläft, sitzt ein durchsichtiges, klickbares Objekt auf der **System-Ebene** (wird vor `lv_layer_top()` und dem Bildschirm durchsucht, deckt also auch den Kontrast-Schleier ab) und frisst diesen ersten Druck; beim Loslassen verschwindet es wieder. Der Web-Spiegel ist ausgenommen: sein Nutzer sieht, wohin er zielt, also weckt `ui_flow_wake_display()` aus der Zeiger-Einspeisung das Panel und nimmt den Blocker weg, *bevor* der Tipp verteilt wird. Dazu ein Sicherheitsnetz zweimal je Sekunde: ein Blocker über hellem Panel würde das Gerät unbedienbar machen und wird deshalb sofort abgeräumt |
| LVGL | Vier Speicher-Callbacks (Geräte, MQTT, Zeit, VPN) bauten die Konfiguration **aus Nullen** neu und schrieben ihre eigenen Default-Literale als Nutzerwahl in NVS — dasselbe Muster, das `gw_max_clients` blockiert hatte | Alle vier starten jetzt bei der **gespeicherten** Konfiguration (wie `rtu_save_cb`) und überschreiben nur, was der Tab wirklich besitzt; der Gerätedialog beim Bearbeiten beim gespeicherten Eintrag. Leere Felder werden als 0 / `""` = „nicht gesetzt" gespeichert, die Defaults liegen im jeweiligen Backend (`normalize_cfg` in `mqtt_fwd.c`, `ntp_client.c`, `wg_client.c`) und werden nur auf die RAM-Kopie angewandt — so wirkt ein geänderter Default auch auf Geräte, die den Wert nie selbst gewählt haben. Zahlen werden geklemmt statt still durch einen Default ersetzt; `field_set()` nullt den Rest des Feldes, damit im NVS-Blob kein Rest eines längeren alten Passworts stehen bleibt |
| Modbus RTU | Jedes Widget im Tab „Mod RTU" speichert bei jeder Änderung die ganze Struktur — ein Dropdown öffnen und denselben Wert wieder wählen kostete einen NVS-Schreibvorgang, den Reset der Master-Statistik und eine leere Deye-Anzeige | `modbus_rtu_set_cfg()` vergleicht mit der laufenden Konfiguration und tut bei Gleichheit **nichts**. Und nur eine geänderte **Bus**-Zeile macht den gelesenen Deye-Wert ungültig; die Bridge-Felder (Port, Busmaske) lassen ihn stehen |
| OTA | **Ein Dateisystem-Update im Schlaf ließ das Panel für immer hell.** `ota_thaw_ui()` schaltet die Beleuchtung wieder ein, die Standby-Logik hielt sich aber weiter für „schlafend" — und ihr Einschlaf-Zweig feuert nicht, wenn sie das schon glaubt. Das Licht blieb also an, bis jemand das Gerät zweimal antippte (nach dem neuen Blocker wäre der erste Tipp zusätzlich verschluckt worden) | `ota_thaw_ui()` meldet den Wechsel jetzt an `ui_flow_wake_display("OTA finished")` (die LVGL-Sperre hält es dabei noch). Der Zustand stimmt damit wieder, und weil niemand das Gerät berührt hat, geht es beim nächsten Timer-Tick von selbst zurück in den Standby. Beim Firmware-Update fällt es nicht auf, weil das direkt neu startet |
| LVGL | Standby war nicht beobachtbar (Panel dunkel, Log still) | Drei Logzeilen auf den Übergängen: `standby: display off after N s`, `... woken by touch (tap swallowed)`, `... display on (<Quelle>, tap kept)` |

**Zwei Fehler, die erst am Gerät auffielen** — beide waren beim Lesen des Codes nicht zu sehen:

* **Zeitzone auf UTC-8.** Die Defaults ins Backend zu ziehen hat `c.tz_idx = NTP_TZ_DEFAULT` aus dem „nichts gespeichert"-Zweig entfernt. `tz_idx` hat aber keinen freien Wert für „nicht gesetzt": Index 0 ist Los Angeles, eine legitime Wahl. Ein Gerät ohne gespeicherte Zeitkonfiguration (genau dieses) sprang damit auf UTC-8, sichtbar als 21:33 des Vortags. Der Erst-Boot-Default gehört in den `!= ESP_OK`-Zweig, nicht in `normalize_cfg` — steht jetzt als Kommentar daneben.
* **Standby weckte sich selbst nach 520 ms.** Der Blocker-Callback hing an `LV_EVENT_ALL`, und das Einblenden des Blockers erzeugt Style-, Layout- und Zeichen-Events — die als „Nutzeraktivität" gewertet wurden. Er hängt jetzt nur an `PRESSED`/`PRESSING`/`RELEASED`/`PRESS_LOST`. Ohne die Logzeilen aus derselben Runde wäre das unentdeckt geblieben: das Panel wäre einfach nie dunkel geworden.

**Nachweis am Gerät** (Steuerung über die Zeiger-Einspeisung des Web-Spiegels, Kontrolle über den MJPEG-Strom):

* Tab „Mod RTU" öffnen: Deye-Wert läuft durch (Polls zählen weiter), kein Aussetzer — vorher blankte ihn schon der Tab-Aufbau.
* Bridge-Häkchen „Bus B" setzen und zurücknehmen: echter NVS-Schreibvorgang, Deye-Wert bleibt trotzdem stehen (`bus_changed == false`).
* Baud Bus A auf denselben Wert (9600) neu wählen: keine Wirkung, Master-Statistik läuft ununterbrochen weiter.
* MQTT-Tab „Speichern" ohne Änderung: Broker, Benutzer, Passwort, Basis-Topic und die drei Schalter unverändert, Verbindung hält, `veröffentlicht` zählt weiter (39 → 49).
* Zeit-Tab „Speichern": UTC+1 Berlin und `pool.ntp.org` bleiben, Uhr stimmt sekundengenau mit dem Entwicklungsrechner.
* VPN-Tab „Speichern" (VPN aus, aber vollständig konfiguriert), Tab neu aufgebaut: Bild **byteidentisch** — Schlüssel, Endpoint, Port unangetastet.
* Gerätedialog „West" öffnen, „Speichern", neu öffnen: alle acht Felder gleich, `/api/devices` unverändert.
* 160 s ohne Eingabe: `standby: display off after 120 s` und danach Ruhe. Tipp im Web-Spiegel im Schlaf: `display on (web pointer, tap kept)` — und die Einstellungen gehen auf, der Tipp wird also *nicht* verschluckt.
* Firmware-OTA über WLAN im Schlaf: 2,1 MB in 19 s, HTTP 200, Neustart in `ota_1`, `fs_build` passend, Bewährung nach 62 s bestätigt, kein SDIO-Abbruch (`dma_largest` durchgehend 61440).
* Dateisystem-OTA im Schlaf: `display on (OTA finished, tap kept)` → `UI thawed` → im selben Tick wieder `display off after 120 s`. Genau so soll es sein: Zustand stimmt, und ohne Berührung wird es wieder dunkel.

Mit dem Finger auf dem Glas vom Betreiber bestätigt (9. September): schlafendes Panel angetippt — es wird hell, der Netz-Sollwert bleibt stehen, und der Slider ist danach normal bedienbar. Das war der einzige Weg, diesen Pfad zu prüfen: der Web-Spiegel ist genau dafür ausgenommen.

## Nachtrag 4 (9. September): der Modbus-Regelpfad

Alle vier Punkte vom Betreiber entschieden und umgesetzt, Build 240 (v1.0.151).

| Punkt | Entscheidung | Wie es jetzt läuft |
| --- | --- | --- |
| 1 — gemeinsame IP | weitergehen + Altersgrenze | Ein Gerät, das nicht antwortet, beendet die Runde seiner IP nicht mehr: die Verbindung wird verworfen, **sofort neu aufgebaut** und mit dem nächsten Gerät weitergemacht. Neu aufgebaut wird immer — nach einem Zeitablauf kann die Antwort noch unterwegs sein und würde sonst dem nächsten Gerät zugeordnet (dieselbe Falle, gegen die `rtu_drain()` auf der Zweidrahtleitung schützt). Scheitert der Aufbau, gelten die restlichen Geräte als offline und die Runde wartet wie bisher 3 s. Netzzähler- und Deye-Zähler-Rollen werden in jeder Runde **zuerst** abgefragt. Dazu: ein Beitrag zählt nur noch, solange sein Gerät antwortet (3 × Abfrageintervall, mindestens 15 s) — vorher blieb der letzte Wert für immer im Energiemodell, die Solaranzeige zeigte nachts die Leistung vom Abend. |
| 2 — Deye-CT | Regelung sperren, Anzeige behalten | Register 619 wird nie mehr zum Regelwert. Dabei kam heraus, dass Regel- und Anzeigewert **dieselbe Variable** waren (`s_st.grid_w`): der Aggregator füllt sie auch aus Ersatzquellen, also konnte der Ersatzwert in `modbus_tcp_grid_w_fresh()` landen, obwohl ein echter Zähler existiert. Der Regelpfad hat jetzt seine eigene Variable (`s_grid_ctrl_w`), die **nur** ein Worker beschreibt, der wirklich ein Gerät mit der Rolle Netzzähler gelesen hat. Ist keines eingerichtet, sagt es das beim Start ins Log. |
| 3 — Zwangsmodus | beim Start auf Normal | Modus und Leistung liegen im NVS, werden beim Start aber nicht wiederhergestellt, sondern **abgeräumt**: findet die Firmware einen gespeicherten Zwang, schreibt sie ~10 s nach dem Hochlaufen aktiv „Normal" in den Wechselrichter (die 10 s geben der Zweidrahtleitung Zeit) und protokolliert es. Ein Zwang läuft zusätzlich nach 2 h von selbst ab. Die entscheidenden Register (142/143, beim Laden 127/128) werden zurückgelesen und verglichen — `reg143 = 5000 verified` bzw. `inverter reports 20000 -- NOT applied`; `/api/deye/live` liefert das unter `ctrl` mit (`checked`, `failed`, `left`). |
| 4 — 0-W-Halten | 60 s, danach stumm | Die Null ist jetzt eine **Brücke mit Ablauf**: nach `slave_hold_s` Sekunden antwortet die Emulation nicht mehr, der Deye erkennt den Zählerausfall und regelt mit seinem eigenen Wandler weiter — eine echte Messung ist besser als eine stille Fehlregelung. Einstellbar unter Mod RTU: unbegrenzt (altes Verhalten) / 30 / 60 / 120 s, Voreinstellung 60 s. Das Feld nutzt das bisherige `_rsv`-Byte der RTU-Konfiguration, das auf jedem Gerät 0 ist — also kein Layout-Wechsel, und 0 heißt „Voreinstellung". `/deye` zeigt die drei Zustände (frisch / Überbrückung / stumm), das Log meldet `meter emulation going SILENT`. |

Mitgenommen, weil es dieselbe Wurzel hat: der **SLS-Exportschutz** rechnete mit dem Anzeige-Aggregat, das ein Ersatzwert sein kann und keine eigene Altersgrenze hat. Er nutzt jetzt denselben frischegeprüften Regelwert wie die Emulation (`MB_GRID_MAX_AGE_MS` liegt dafür jetzt in `modbus_tcp.h`, eine Definition für beide). Damit ist auch der Fund „SLS-Exportschutz rechnet mit veralteten Daten" erledigt.

Die beiden sich widersprechenden Kommentare bei `modbus_rtu.c` (einer sagte „stumm bleiben", der andere „immer antworten") sind aufgelöst — die Beschreibung entspricht jetzt dem Code.

**Nachweis am Gerät:** Build 240 geflasht, sauberer Start; Zähler-Emulation antwortet weiter (1848 Anfragen in 3 min, Alter 46 ms), Deye-Werte laufen durch, MQTT und Uhr in Ordnung. `/api/meter` liefert die neuen Felder (`quiet: 0`, `hold: 60`, `stale: 0`), `/api/deye/live` den `ctrl`-Block (`mode Normal`, `failed 0`) — das JSON bleibt mit 671 von 1600 Byte im Rahmen. Drei Minuten Log ohne einen einzigen Lesefehler am Netzzähler, also keine Gefahr, dass die 60-s-Brücke im Normalbetrieb überhaupt anspricht. Kein gespeicherter Zwangsmodus vorhanden, das Abräumen beim Start war deshalb nicht zu sehen.

**Zwei Dinge stehen noch aus, weil sie einen Eingriff am laufenden Wechselrichter brauchen:**

* **Was der Deye bei Zählerausfall wirklich tut.** Die Stummschaltung ist nur zu prüfen, indem man den Netzzähler länger als 60 s ausfallen lässt. Das ist ein echter Eingriff (der Deye meldet dann einen Zählerfehler und wechselt auf seinen Wandler) und wartet auf ausdrückliches „ja".
* **Die Reihenfolge bei gemeinsamer IP.** Bei diesem Aufbau scheitert West (Unit 2) *nach* Ost (Unit 1), das entscheidende „ein Ausfall vor einem funktionierenden Gerät" kommt also nicht vor. Nachstellen ließe es sich nur mit einem erfundenen Gerät in der Liste des Betreibers — dafür wurde nichts an seiner Konfiguration verbogen.

## Gut gemacht — nicht anfassen

* Frische-Schranke des Netzwerts (`modbus_tcp_grid_w_fresh`): nur ein echter erfolgreicher Read setzt den Zeitstempel, `reconfigure_apply()` invalidiert bewusst, überlaufsichere Zeitarithmetik. Sicherheitsschienen der Manipulation: Hauptschalter aus, nur bei frischem Zähler, NaN abgewiesen, ±100 kW geklemmt, seiteneffektfreies `compute_served()`.
* Ein Eigentümer pro UART: alles, was auf einen RS485-Bus will, wird zwischen den Polls bedient; `modbus_rtu_txn()` ist ein Lehrbuchbeispiel (Sperre pro Bus, verspätete Signale verworfen, Nachfrist vor dem Freigeben). Bridge lehnt Slave-Busse ab.
* OTA-Empfangspuffer garantiert im internen RAM (Quelle eines Flash-Schreibvorgangs), Oberfläche eingefroren und Hintergrundlicht aus während des Schreibens, `recovery.html` im App-Image statt im Dateisystem, Reset-Grund und Speicherstände in `/ota`.
* Jeder `lv_*`-Aufruf aus fremden Tasks unter `app_lvgl_lock()` mit Timeout; keine Sperr-Reihenfolge-Umkehr; Timer werden mit ihren Popups gelöscht; `ui_settings_create()` bewusst nach den Backend-Loads.
* Socket-Haushalt: DNS-Socket nur bei aktivem AP, `max_open_sockets=4` mit LRU, `TCP_MSL=5000`; Timer-Callback → Notify → eigener Worker für esp_hosted-RPCs.
* `esp_hosted ==2.12.8` fest gepinnt mit dem Beweis daneben; die gescheiterten Experimente stehen in `sdkconfig.defaults`, damit sie niemand wiederholt.

## Werkzeug für die Web-Oberfläche

Für die Responsive-Arbeit ist ein kleiner Entwicklungs-Proxy entstanden (Python, ~60 Zeilen): er liefert eine lokale Arbeitskopie von `deye.html` aus, reicht alle API-Aufrufe ans Gerät durch und stellt unter `/wrap?w=390` einen Iframe in Telefonbreite bereit. Headless Chrome erzeugt daraus Screenshots und misst, welche Elemente über den Viewport hinausragen — ohne für jede CSS-Änderung zu flashen. Wichtig dabei: Headless Chrome hat eine Mindestfensterbreite von 500 px; ohne den Iframe misst man Unsinn.

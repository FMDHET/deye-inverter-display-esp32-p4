# Code-Review September 2026

Vollständige Durchsicht der Firmware (rund 13 400 Zeilen C, HTML und Python) am 8. September 2026, aufgeteilt auf fünf Teilbereiche: OTA und Webserver, Modbus-Regelpfad, Kern (Boot, WLAN, NVS, MQTT, NTP, WireGuard), LVGL-Oberfläche und die eingebetteten Webseiten. Jeder Bereich wurde Zeile für Zeile gelesen; die Funde wurden am angeschlossenen Gerät nachgestellt, wo das möglich war.

Diese Seite hält fest, **was behoben wurde** (mit Nachweis), **was offen ist** (nach Dringlichkeit) und **was gut ist und so bleiben soll**. Die offenen Punkte im Regelpfad sind bewusst nicht „nebenbei" geändert worden: sie betreffen das Verhalten gegenüber dem Wechselrichter und brauchen eine Entscheidung des Betreibers.

## Zusammenfassung

* **OTA über WLAN ist repariert.** Ursache war der WLAN-Treiber esp_hosted, der seine SDIO-Empfangspuffer aus dem knappen internen DMA-Speicher holte und bei Knappheit abstürzte statt einen Fehler zu melden. Mit `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` kommen die Puffer aus dem PSRAM. Messreihe: 10 Firmware- und 3 Dateisystem-Updates hintereinander, null Abbrüche (vorher etwa jeder vierte).
* **Sieben weitere Fehler im OTA-Pfad behoben**, darunter zwei, die das Gerät lahmlegen konnten: ein Upload, der mitten drin stehen bleibt, fror das Gerät dauerhaft ein; eine falsche Datei im Firmware-Feld löschte den einzigen Rückfall-Abschnitt.
* **Die Web-Oberfläche `/deye` ist jetzt auf dem iPhone benutzbar**: Tabellenzeilen werden zu Karten, die eingegebenen Werte werden vor dem Senden geprüft, das Polling stapelt sich nicht mehr.
* **Offen und wichtig:** vier Punkte im Modbus-Regelpfad (siehe unten), die Sicherheit des Notfall-WLANs, und zwei kleine Fehler in der Display-Oberfläche, die das Gerät unbedienbar machen können (Touch-Ausfall beim Start, Helligkeit 0 %).

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

### Modbus-Regelpfad (Entscheidung des Betreibers nötig)

Diese Punkte betreffen, was der Wechselrichter zu sehen bekommt. Sie sind absichtlich nicht geändert worden.

1. **Gemeinsame IP: ein ausgefallenes Gerät blockiert den Netzzähler.** `modbus_tcp.c` ~721–759: Beim ersten Fehler in der Geräteliste einer IP schließt der Worker den Socket, bricht die Runde ab und schläft 3 s. Fronius-Aufbau mit Smart Meter (Unit 240) und Wechselrichter (Unit 1) auf *derselben* IP: nachts schaltet der Symo ab, die Reads an Unit 1 laufen in den Timeout, der Zähler wird nie mehr gelesen, nach 12 s gilt er als veraltet, die Emulation sendet 0 W — die ganze Nacht. Vorschlag: nur bei Transportfehlern abbrechen, bei Modbus-Exceptions zum nächsten Gerät weitergehen; Netzzähler-Rollen zuerst abfragen.
2. **Deye-CT als „frischer" Netzwert = Rückkopplung.** `modbus_tcp.c` ~746: Ohne Netzzähler-Rolle wird der CT-Eingang des Deye (Register 619) als gültiger Netzwert übernommen. Das ist aber genau der Wert, den unsere Emulation ihm zuletzt geschickt hat — die Schleife füttert sich selbst. Vorschlag: `s_grid_valid` nie aus `deye_ct` setzen; Anzeige ja, Regelung nein.
3. **Erzwungener Akku-Modus überlebt keinen Neustart des Displays — der Wechselrichter behält ihn aber.** `deye_ctrl.c`: Modus nur im RAM, Rückgabewerte der zwölf Schreibbefehle werden verworfen, nichts wird zurückgelesen. Nach einem OTA sagt das Display „Normal", der Deye lädt weiter mit 5 kW aus dem Netz. Vorschlag: Modus mit Zeitstempel in NVS, beim Start zurücksetzen oder wiederherstellen, Maximaldauer mit automatischem Rückfall, Schreibbefehle verifizieren.
4. **0-W-Halten bei veraltetem Zähler ist zeitlich unbegrenzt.** `modbus_rtu.c` ~292 und ~325 (zwei sich widersprechende Kommentare). Zehn Minuten Router-Neustart bei 5 kW Entladung: der Deye hält 5 kW, egal wie sich die Last ändert. Ein *stummer* Zähler würde stattdessen die Zählerausfall-Behandlung des Deye auslösen. Vorschlag: 0 W nur als kurze Überbrückung (30–60 s), danach nicht mehr antworten; am Gerät prüfen, was der Deye bei Zählerausfall tut.

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
* Der Aufweck-Tipp wird an das Widget darunter durchgereicht — links liegt der Netz-Sollwert-Slider über die volle Höhe.
* Vier Speicher-Callbacks (Geräte, MQTT, NTP, VPN) bauen die Struktur aus Nullen neu und schreiben Default-Literale als Nutzerwahl in NVS — das Muster, das schon `gw_max_clients` blockiert hatte.
* VPN-Tastatur schwebt über andere Tabs; jeder RTU-Dropdown-Tick schreibt synchron in NVS und blendet die Deye-Anzeige aus; Scan-Liste kann bei „scanne…" hängen; Deye-Leistungsslider 0–22000 gegen Backend 1000–20000; kein `max_length` auf Textfeldern (40-Zeichen-MQTT-Passwort wird stumm auf 39 gekürzt).

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

## Gut gemacht — nicht anfassen

* Frische-Schranke des Netzwerts (`modbus_tcp_grid_w_fresh`): nur ein echter erfolgreicher Read setzt den Zeitstempel, `reconfigure_apply()` invalidiert bewusst, überlaufsichere Zeitarithmetik. Sicherheitsschienen der Manipulation: Hauptschalter aus, nur bei frischem Zähler, NaN abgewiesen, ±100 kW geklemmt, seiteneffektfreies `compute_served()`.
* Ein Eigentümer pro UART: alles, was auf einen RS485-Bus will, wird zwischen den Polls bedient; `modbus_rtu_txn()` ist ein Lehrbuchbeispiel (Sperre pro Bus, verspätete Signale verworfen, Nachfrist vor dem Freigeben). Bridge lehnt Slave-Busse ab.
* OTA-Empfangspuffer garantiert im internen RAM (Quelle eines Flash-Schreibvorgangs), Oberfläche eingefroren und Hintergrundlicht aus während des Schreibens, `recovery.html` im App-Image statt im Dateisystem, Reset-Grund und Speicherstände in `/ota`.
* Jeder `lv_*`-Aufruf aus fremden Tasks unter `app_lvgl_lock()` mit Timeout; keine Sperr-Reihenfolge-Umkehr; Timer werden mit ihren Popups gelöscht; `ui_settings_create()` bewusst nach den Backend-Loads.
* Socket-Haushalt: DNS-Socket nur bei aktivem AP, `max_open_sockets=4` mit LRU, `TCP_MSL=5000`; Timer-Callback → Notify → eigener Worker für esp_hosted-RPCs.
* `esp_hosted ==2.12.8` fest gepinnt mit dem Beweis daneben; die gescheiterten Experimente stehen in `sdkconfig.defaults`, damit sie niemand wiederholt.

## Werkzeug für die Web-Oberfläche

Für die Responsive-Arbeit ist ein kleiner Entwicklungs-Proxy entstanden (Python, ~60 Zeilen): er liefert eine lokale Arbeitskopie von `deye.html` aus, reicht alle API-Aufrufe ans Gerät durch und stellt unter `/wrap?w=390` einen Iframe in Telefonbreite bereit. Headless Chrome erzeugt daraus Screenshots und misst, welche Elemente über den Viewport hinausragen — ohne für jede CSS-Änderung zu flashen. Wichtig dabei: Headless Chrome hat eine Mindestfensterbreite von 500 px; ohne den Iframe misst man Unsinn.

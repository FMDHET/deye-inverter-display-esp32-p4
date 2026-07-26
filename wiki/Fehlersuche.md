# Fehlersuche

## Erste Anlaufstellen

| Weg | Was man sieht |
| --- | --- |
| Serieller Monitor | `pio device monitor -e guition-p4` — mit Exception-Decoder und Zeitstempeln |
| `GET /ota` | Version, Build, FS-Build, aktiver Slot, Laufzeit |
| `GET /api/live` | alle Messwerte, MQTT- und NTP-Zustand |
| `GET /api/devices` | Verbindungszustand und Werte pro Modbus-TCP-Gerät |
| Tab-Kopfzeilen | jeder Einstellungs-Tab zeigt oben eine Livezeile mit Zählern |

## Display

**Bild bleibt schwarz, Gerät bootet aber**
Standby ist aktiv (Tab „Display") — einmal antippen. Andernfalls Helligkeit auf 0 %?

**Bild verzerrt, gestreift oder versetzt**
DSI-Timings oder Rotation passen nicht zum Panel. Werte in `main/board_jc4880p443c.h` gegen die Referenz prüfen. Für dieses Modul sind sie verifiziert; bei einer anderen Panelrevision können sie abweichen.

**Umlaute erscheinen als Kästchen**
`CONFIG_LV_CACHE_DEF_SIZE` ist 0 oder zu klein. Tiny-TTF rastert zur Laufzeit und braucht den Cache, sonst fällt LVGL auf die Bitmap-Schrift ohne Umlaute zurück.

**Panel flackert beim Flashen**
Sollte nicht passieren — das UI-Einfrieren samt Backlight-Abschaltung ist eingebaut. Tritt es doch auf, hat `ota_freeze_ui()` den LVGL-Lock nicht bekommen (Zeitüberschreitung nach 2 s); im Log steht `lvgl_lock=0`.

## WLAN

**Bootschleife, sobald WiFi initialisiert wird**
Die Firmware des ESP32-C6-Slave passt nicht zum Host. Hier läuft Slave **2.12.8** zu `esp_hosted ~2.12.0`. Zum Eingrenzen `DEYE_ENABLE_WIFI` in `main.c` auf 0 setzen: bootet das Gerät dann durch, ist es der C6.

**Kein Netz gefunden, obwohl vorhanden**
Nur 2,4 GHz ist zuverlässig erreichbar, wenn das Gerät weit vom Router steht. Ein eigenes 2,4-GHz-Netz hilft mehr als jede Softwareeinstellung.

**Captive Portal öffnet sich nicht von selbst**
Manche Betriebssysteme merken sich, dass ein Netz kein Portal hat. `http://192.168.4.1` direkt aufrufen.

**Gerät nach Update nicht erreichbar**
Zugangsdaten liegen in NVS und überleben Updates — es sei denn, das Partitionslayout wurde verändert. Dann per USB neu einrichten.

## Modbus-TCP

**Gerät bleibt `offline`**

1. IP, Port und Slave-ID prüfen — mehrere Slave-IDs auf einer IP sind erlaubt und üblich (z. B. mehrere Wechselrichter an einem Datalogger).
2. Timeout erhöhen: manche Datalogger antworten erst nach über einer Sekunde.
3. Von einem Rechner im gleichen Netz gegenprüfen, ob Port 502 offen ist.

**Werte sind `nan` oder absurd groß**
Fast immer das falsche Hersteller-Profil. Der häufigste Fall: ein **Eltako DSZ16DZE** ist als Fronius oder als SDM630-float eingetragen. Der Eltako liefert auf `0x0034` **int32**, dort wo ein SDM630 float32 führt.

**Log zeigt `implausible ... skipped`**
Die Plausibilitätsprüfung hat einen Wert über 100 kW oder ein `nan` verworfen. Meist falsches Profil, gelegentlich eine gestörte Verbindung.

**PV zu hoch, Akku fehlt bei einem Fronius-Hybrid**
Der Hybrid wird als reiner String-Wechselrichter behandelt, weil SunSpec-Modell 124 nicht gefunden wurde. Prüfen, ob das Gerät das Speichermodell überhaupt anbietet — sonst wird die Akku-Entladung als PV gezählt.

**Ein Knoten zeigt `--`**
Genau so gedacht: die Quelle ist weg (Gerät deaktiviert, entfernt oder Wert veraltet). Besser als ein eingefrorener Wert.

## Modbus-RTU

**Bus A oder B bleibt `offline`**

1. **Selbsttest** ausführen (Tab „Mod RTU"): A-TX → B-RX und A-RX → B-TX verdrahten. Läuft der durch, sind Transceiver und Firmware in Ordnung und das Problem liegt beim Wechselrichter oder der Strecke dorthin.
2. A und B vertauscht? Der häufigste Verdrahtungsfehler.
3. Gemeinsame Masse zwischen Display und Wechselrichter vorhanden?
4. Baudrate und Slave-ID gegen die Einstellung im Wechselrichter prüfen (Standard 9600).
5. 120 Ω an beiden Busenden.

**Fehlerzähler wächst schnell**
Abschluss, Masse oder Baudrate. Ein langsam mitwachsender Zähler ist auf RS485 dagegen normal.

**Der Deye meldet „Zähler verloren"**
Die Emulation antwortet nicht — Bus deaktiviert, falsche Slave-ID oder falscher Bus als Slave konfiguriert. Der Slave-Zähler im Tab-Kopf muss steigen.

**Der Deye regelt nicht auf den Sollwert**
Die Emulation meldet 0 W, weil kein **frischer** Netzmesswert vorliegt. Ursachen: kein Gerät mit Rolle `Netz-Zaehler` konfiguriert, dieses Gerät offline, oder die Konfiguration wurde gerade gespeichert (dann gilt der Wert bis zum ersten Lesen als ungültig). Das ist die eingebaute Sicherung, kein Fehler — siehe unten.

## Deye-Steuerung

**Modus wird gesetzt, der Wechselrichter reagiert nicht**

1. Läuft ein Bus als **Master**? Ohne Master gibt es keinen Schreibweg.
2. Mit `/deye` prüfen, ob die Register den erwarteten Wert tragen (`142/6` und `166/12`).
3. Modell und Firmwarestand: die Registerbelegung ist an einem SG04LP3 ermittelt.

**Zwangsladung lädt nicht mit der eingestellten Leistung**
Der Wechselrichter reagiert nicht auf die Wattangabe in Register 126, sondern auf den Ladestrom in Register 128. Die Umrechnung `A = W / 50` unterstellt etwa 50 V Batteriespannung — bei anderer Systemspannung stimmt sie nicht.

**Zwangsentladung bricht ein oder wird gedrosselt**
Der [SLS-Export-Schutz](Deye-Steuerung#sls-export-schutz) greift. Im Log:
`SLS guard: export 22400 W > limit 21735 W (SLS 35A) -- throttle → 18000 W`
Der Nutzer-Sollwert bleibt dabei erhalten und wird wiederhergestellt, sobald die Einspeisung sinkt.

**Anzeige friert beim Umschalten des Modus ein**
Sollte nicht passieren: die Schreibzugriffe laufen in der `deye_ctrl`-Task, nicht auf der UI-Task. Tritt es doch auf, wurde ein Modbus-Schreibaufruf direkt aus einem LVGL-Callback gemacht.

## Die 15-kW-Geschichte

Einmal hat diese Anlage **15 kW ins Netz eingespeist**, weil die Eastron-Emulation einen eingefrorenen Netzmesswert weitergemeldet hat: die TCP-Poll-Task hing, der letzte Wert blieb stehen, und der Deye regelte mit voller Leistung gegen eine Zahl, die sich nicht mehr bewegte.

Die Konsequenz steht heute im Code:

* Die Emulation fragt `modbus_tcp_grid_w_fresh(&w, 12000)`.
* Bei `false` meldet sie **0 W** — nicht den letzten Wert, aber sie **antwortet weiterhin** (Schweigen würde der Deye als Zählerausfall werten und auf seinen eigenen CT umschalten).
* Nach jedem Speichern der Gerätekonfiguration ist der Netzwert ungültig, bis er frisch gelesen wurde.

> [!WARNING]
> Wer an `modbus_rtu.c` oder am Netzpfad in `modbus_tcp.c` arbeitet: diese Kopplung muss erhalten bleiben. Ein Regelkreis mit einem stehenden Messwert ist gefährlicher als einer ohne Messwert.

## Build und Flash

**`BUILD MISMATCH: firmware #146 but filesystem #145`**
Firmware und Dateisystem sind getrennt geflasht worden. Beides in einem Aufruf: `pio run -e guition-p4 -t upload -t flashfs`. Über WiFi beide Images schreiben und neu starten.

**`Filesystem build unavailable (asset image not flashed?)`**
Die `storage`-Partition ist leer oder unlesbar. `-t flashfs` bzw. `POST /ota/fs` nachziehen. Unkritisch: die Firmware läuft weiter.

**OTA-Upload wird abgelehnt**

* `bad size` — Image größer als die Partition
* `image invalid` — beschädigtes oder für ein anderes Ziel gebautes Image
* Abbruch ohne Meldung — früher durch Socket-Erschöpfung verursacht; dagegen steht `CONFIG_LWIP_TCP_MSL=5000` in `sdkconfig.defaults`

**Nach dem Update verhält sich die Firmware falsch**
`POST /ota/rollback` schaltet auf den vorigen Slot zurück, oder die Recovery-Seite unter `/recovery` benutzen.

**Firmware läuft nicht auf ECO2-Silizium**
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` muss gesetzt sein, sonst enthält das Binary `xespv2p2`-Befehle, die ältere Revisionen nicht dekodieren.

## MQTT

**Verbindet nicht**
Host, Port und Zugangsdaten prüfen; `mqtt_conn` in `/api/live` zeigt den Zustand. Broker, die eine Authentifizierung erzwingen, brechen ohne Zugangsdaten sofort ab.

**Entities erscheinen nicht in Home Assistant**
„HA Discovery" muss aktiv sein. Die Konfigurations-Topics werden nach dem Verbinden gesendet — Discovery einschalten, speichern, dann verbindet sich der Client neu und veröffentlicht sie.

**Werte bleiben nach einem Neustart stehen**
Retain-Flag. Gewollt, wenn Home Assistant beim Start sofort Werte sehen soll; abschaltbar im Tab „MQTT".

## VPN

**Tunnel kommt nicht zustande**
Die häufigste Ursache: die Uhr ist nicht gestellt. Der WireGuard-Handschlag braucht eine plausible Wanduhrzeit — SNTP muss aktiv und der Server erreichbar sein. Danach Schlüssel, Endpunkt und Port prüfen und `AllowedIPs` auf der Gegenseite gegen die Tunnel-IP abgleichen.

**Tunnel bricht regelmäßig ab**
Keepalive setzen (25 s), wenn das Gerät hinter NAT steht.

## Einstellungen

**Gespeicherte Werte scheinen verloren**
Wenn ein Tab leer rendert und ein Speichern die Konfiguration überschreibt, wurde `ui_settings_create()` **vor** den Backend-Starts aufgerufen. In `main.c` muss es am Ende stehen — siehe [Architektur](Architektur#bootreihenfolge).

**Absturz beim Start, kurz nach dem RTU-Start**
`deye_ctrl_start()` läuft vor `modbus_rtu_start()`. Die Schreibtask braucht den Anfrage-Mutex des Busses.

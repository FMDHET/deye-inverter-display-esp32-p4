# Fehlersuche

## Wo man zuerst hinschaut

Bevor man rät, holt man sich Informationen. Es gibt sechs Quellen:

| Quelle | Was man sieht |
| --- | --- |
| **Serieller Monitor** | `pio device monitor -e guition-p4` — alle Meldungen der Firmware mit Zeitstempel. Bei Abstürzen wird sogar die Fehlerstelle im Code aufgelöst. |
| **`GET /log`** | dasselbe Log, aber **ohne Kabel**: die letzten 48 kB Meldungen aus einem Ringpuffer im PSRAM. `?tail=2000` für nur das Ende, `?clear=1` zum Leeren danach. Überlebt keinen Neustart — dafür ist der Coredump da. |
| **`GET /coredump`** | der letzte Absturz als Abbild für `espcoredump.py`. `GET /ota` nennt vorher schon Task, Programmzähler und Grund im Klartext. |
| **`GET /ota`** | Version, Build-Nummer, welcher Speicherabschnitt läuft, Laufzeit, **Grund des letzten Neustarts**, freier Speicher |
| **`GET /api/live`** | alle Messwerte, MQTT- und Uhr-Zustand |
| **`GET /api/devices`** | pro Gerät: antwortet es, und mit welchen Werten |
| **`GET /api/deye/live`** | was der Wechselrichter über sich selbst meldet, und welche Registerblöcke gerade antworten (`blocks`) |
| **Kopfzeilen der Menüs** | jeder Reiter zeigt oben laufende Zähler — oft steht die Antwort schon dort |
| **Die Zeile unten am Hauptbildschirm** | erscheint nur, wenn etwas nicht stimmt — und sagt dann, was |

Die zwei neuen Quellen sind der Grund, warum man an ein Gerät an der Wand überhaupt herankommt:

```bash
curl -s http://<ip>/log?tail=3000          # was ist gerade passiert
curl -s http://<ip>/ota | jq .coredump     # was war beim letzten Absturz
```

Wer nicht jedes Mal überlegen will, welche Zahl wo steht und ab wann sie schlecht ist, nimmt den Sammelblick:

```bash
python3 scripts/health.py                  # alles Wichtige in acht Zeilen
python3 scripts/health.py --log 40         # dazu das Ende des Logs
```

Er fragt alle vier Endpunkte ab, sagt „alles unauffällig" oder listet auf, was auffällt (Absturz, Coredump, knapper DMA-Speicher, stummer Zähler, fehlende Geräte, aktive Phasenmanipulation), und liefert einen Rückgabewert ≠ 0, wenn etwas ansteht — damit taugt er auch für einen Cronjob.

### Die Hinweiszeile auf dem Hauptbildschirm

Sie ist versteckt, solange alles stimmt. Was sie zeigen kann, von dringend nach harmlos:

| Zeile | Was los ist |
| --- | --- |
| „Deye fragt den Zähler seit *n* min nicht mehr — Gerät stromlos machen" | Der Slave-Bus ist taub. Genau der Fall vom 8. September: der Empfänger hing auf 0, und **nur ein Kaltstart** half — Warmstarts nicht. |
| „Kein Netzmesswert seit *n* s — der Deye regelt mit seinem eigenen Wandler" | Die Überbrückung ist abgelaufen, die Emulation schweigt absichtlich. Ursache beim Netzzähler suchen, nicht am Display. |
| „Phasenmanipulation aktiv (noch *n* min)" | Kein Fehler, sondern eine Erinnerung: der Deye bekommt gerade verbogene Werte. |
| „*n* von *m* Geräten antworten nicht — Werte unvollständig" | Ein Gerät schweigt. Die angezeigten Summen fehlt sein Beitrag; wo gar nichts bekannt ist, steht „--" statt einer Zahl. |

Ein Tipp zur Laufzeit: tippe auf die Uhr. Steht dort nur eine kurze Zeit, hat das Gerät sich neu gestartet. Dann sagt das Feld `reset` in `GET /ota`, **warum** — an einem Gerät an der Wand gibt es keine serielle Konsole, und ohne diese Auskunft bleibt nur Raten:

```bash
curl -s http://<ip>/ota
# ... "reset":"PANIC","heap":29542723,"heap_min":29538820
```

`SW` ist unser eigener Neustart nach einem Update und harmlos, ebenso `POWERON`, `EXT` und `USB`. `PANIC`, `TASK_WDT`, `INT_WDT` und `BROWNOUT` sind Befunde — die Bedeutung steht unter [OTA und Recovery](OTA-und-Recovery#was-schiefgehen-kann). Wer den Grund kennt, weiß auch, wo sich der Blick in den seriellen Monitor lohnt.

## Bildschirm

**Bild bleibt schwarz, das Gerät läuft aber.**
Wahrscheinlich der Standby: nach einer eingestellten Zeit ohne Berührung geht die Beleuchtung aus. Einmal antippen — dieser erste Tipp weckt nur auf und löst nichts aus, also ruhig irgendwo auf den Bildschirm. Sonst prüfen, ob die Helligkeit ganz unten steht (unter 5 % geht sie nicht mehr). Am Serial-Log ist der Standby mitzulesen: `ui_flow: standby: display off after 120 s` beim Einschlafen, `... woken by touch` beim Aufwecken.

**Bild verzerrt, gestreift oder verschoben.**
Die Zeitwerte für die Bildschirmansteuerung passen nicht zum Panel. Sie stehen in `main/board_jc4880p443c.h` und sind für dieses Modul erprobt — bei einer anderen Panel-Revision können sie abweichen. Selbst herumprobieren ist hier mühsam; besser die Werte aus einer bekannten funktionierenden Quelle übernehmen.

**Umlaute erscheinen als leere Kästchen.**
Der Zwischenspeicher für Schriftzeichen ist zu klein oder aus. Die Buchstaben werden zur Laufzeit aus einer Schriftdatei berechnet und brauchen Platz dafür; ohne ihn fällt LVGL lautlos auf die eingebaute Bildschriftart zurück, und die kennt keine Umlaute. Lösung: `CONFIG_LV_CACHE_DEF_SIZE` muss gesetzt sein (hier 262144). Der Standardwert ist 0.

**Das Panel flackert beim Firmware-Update.**
Sollte nicht passieren, denn Oberfläche und Beleuchtung werden dafür abgeschaltet. Wenn doch: die Sperre auf dem Bildschirmspeicher wurde nicht rechtzeitig frei (Abbruch nach 2 Sekunden). Im Protokoll steht dann `lvgl_lock=0`.

## WLAN

**Das Gerät startet immer wieder neu, sobald WLAN dazukommt.**
Die Programme auf Haupt- und Funkchip passen nicht zueinander. Hier läuft auf dem Funkchip Version **2.12.8** zu `esp_hosted` 2.12.x. Zum Eingrenzen in `main.c` `#define DEYE_ENABLE_WIFI 1` auf `0` setzen — startet es dann durch, ist es bestätigt.

**Das eigene WLAN wird nicht gefunden.**
Weiter weg vom Router ist nur 2,4 GHz zuverlässig; 5 GHz kommt durch Wände viel schlechter. Ein eigenes 2,4-GHz-Netz hilft mehr als jede Softwareeinstellung. Zur Einschätzung der Signalstärke: [WLAN-Seite](WLAN-und-Captive-Portal#signalstärke-verstehen).

**Das Anmeldefenster springt nicht auf.**
Manche Betriebssysteme merken sich, dass ein Netz kein Portal hat. Einfach `http://192.168.4.1` direkt im Browser aufrufen.

**Nach einem Update ist das Gerät nicht mehr erreichbar.**
Zugangsdaten liegen in einem Speicherbereich, der Updates übersteht — es sei denn, die Speicheraufteilung selbst wurde geändert. Dann hilft nur die Ersteinrichtung per USB.

## Geräte im Netzwerk (Modbus-TCP)

**Ein Gerät bleibt `offline`.** In dieser Reihenfolge prüfen:

1. IP-Adresse, Port und Slave-ID. Mehrere Slave-IDs auf derselben IP sind normal — bei Wechselrichtern hinter einem gemeinsamen Datenlogger sogar der Regelfall.
2. Timeout hochsetzen. Manche Datenlogger brauchen über eine Sekunde für eine Antwort.
3. Von einem Rechner aus prüfen, ob Port 502 überhaupt offen ist. Manche Geräte müssen Modbus erst in ihrem eigenen Menü freigeschaltet bekommen.
4. Steht der Schalter „aktiv" auf ein?

**Werte sind `nan` oder absurd groß.**
Das ist fast immer das falsche Hersteller-Profil. Der klassische Fall: ein **Eltako-Zähler**, eingetragen als Fronius oder als SDM630. Der Eltako legt seine Leistung als ganze Zahl in dasselbe Register, in dem ein SDM630 eine Fließkommazahl hat. Dieselbe Adresse, komplett andere Bedeutung der Bits — und es gibt keine Fehlermeldung, nur Unsinn. Ausführlich: [Modbus-TCP](Modbus-TCP#die-falle-mit-dem-eltako-zähler).

**Im Protokoll steht `implausible ... skipped`.**
Die Plausibilitätsprüfung hat einen Wert über 100 kW oder keine gültige Zahl verworfen. Auch hier: meist das falsche Profil, seltener eine gestörte Verbindung.

**Solarleistung zu hoch, Akku fehlt bei einem Fronius-Hybrid.**
Das Gerät wird als normaler Wechselrichter behandelt, weil der Speicher-Block (SunSpec-Modell 124) nicht gefunden wurde. Dadurch wird die Akku-Entladung als Solarleistung mitgezählt — abends sieht man dann „Sonnenstrom" ohne Sonne. Prüfen, ob das Gerät diesen Block überhaupt anbietet. Erklärung: [Modbus-TCP](Modbus-TCP#ein-hybrid-rechnet-anders).

**Ein Kreis zeigt `--`.**
Das ist kein Fehler, sondern Absicht: die Quelle ist weg (Gerät aus, entfernt oder Wert zu alt). Ein eingefrorener alter Wert wäre schlimmer, weil er wie ein aktueller aussieht.

## Zweidrahtleitung (Modbus-RTU)

**Ein Bus bleibt `offline`.** Der Reihe nach:

1. **Erst den Selbsttest laufen lassen** (Reiter „Mod RTU"). Dafür Bus A und B gegeneinander verdrahten: A-senden → B-empfangen und A-empfangen → B-senden. Läuft der Test durch, sind Transceiver und Software in Ordnung, und das Problem liegt auf der Strecke zum Wechselrichter oder in dessen Einstellungen. Das spart viel Ratearbeit.
2. **A und B vertauscht?** Der häufigste Verdrahtungsfehler überhaupt, und das Symptom ist immer dasselbe: gar nichts.
3. **Gemeinsame Masse** zwischen Display und Wechselrichter vorhanden?
4. **Baudrate und Slave-ID** mit den Einstellungen im Wechselrichter vergleichen (üblich 9600).
5. **120 Ω an beiden Leitungsenden** zwischen A und B.

**Der Fehlerzähler wächst schnell.**
Ein langsam mitwachsender Zähler ist auf so einer Leitung völlig normal — elektrische Störungen gibt es immer, und eine kaputte Nachricht wird einfach weggeworfen. Wächst er aber ähnlich schnell wie der Zähler der erfolgreichen Abfragen, stimmt etwas mit Abschlusswiderständen, Masse, Baudrate oder Leitungsführung nicht.

**Der Deye meldet „Zähler verloren".**
Unser gefälschter Zähler antwortet nicht. Prüfen: ist der Bus eingeschaltet, steht er auf *Slave*, und stimmt die Slave-ID? Der Slave-Zähler im Menükopf muss steigen — wenn nicht, kommen die Fragen des Deye nicht bei uns an. Auf `/deye` heißt derselbe Zähler „Anfragen vom Deye"; im Normalbetrieb zählt er mehrmals pro Sekunde.

**Der Zähler bleibt bei 0, obwohl Verdrahtung und Einstellungen stimmen — und ein Neustart hilft nicht.**
Dann das Display einmal **komplett stromlos** machen (Stecker ziehen, nicht nur neu starten). Das ist einmal genau so passiert: nach einer minutenlangen Boot-Schleife (fehlgeschlagenes Firmware-Experiment) blieb der Empfänger des Slave-Transceivers dauerhaft auf 0 hängen — die Leitung zeigte keine einzige Flanke mehr, auch nicht, als die Firmware ihre Pins komplett freigab. Rund 40 Warmstarts (Reset-Taste, Neustart nach Update) änderten daran nichts; Strom weg, Strom dran, und der Deye fragte sofort wieder. Der Master-Bus auf dem zweiten Transceiver lief die ganze Zeit — dass sich Register lesen lassen, sagt also nichts über den Zähler-Bus aus.

**Der Deye regelt nicht auf den eingestellten Sollwert.**
Sehr wahrscheinlich meldet die Emulation gerade 0 Watt, weil **kein frischer Netzmesswert** vorliegt. Mögliche Ursachen: kein Gerät mit der Rolle `Netz-Zaehler` eingerichtet, dieses Gerät antwortet nicht, oder die Geräteliste wurde eben gespeichert (dann gilt der Wert bis zum ersten Lesen absichtlich als ungültig). Fehlt die Rolle ganz, steht es beim Start im Log: `no device has role 'Netz-Zaehler' -- no grid value for the meter emulation`.

Das ist **kein Fehler, sondern die eingebaute Sicherung.** Warum, steht gleich unten.

**Der Deye meldet Alarm, und unser Zähler antwortet nicht mehr.**
Wenn der Netzmesswert länger als die eingestellte Überbrückung (Voreinstellung 60 s) fehlt, **verstummt die Emulation absichtlich**. Der Deye geht dann in Betriebszustand 3 (Alarm, Register 500) und regelt mit seinem eigenen Stromwandler weiter — eine echte Messung ist besser als unser „0 Watt", denn dabei driftet er blind (gemessen: 1,3 kW Akkuleistung in einer Minute, ohne dass ihm jemand etwas gesagt hätte).

Auf `/deye` steht im Zähler-Tab, in welchem Zustand die Emulation ist: *frisch*, *Überbrückung* oder *stumm*. Im Log:

```text
W mb_rtu: grid reading STALE -> meter reports 0 W (bridge; Deye holds)
W mb_rtu: grid reading stale for >60 s -- meter emulation going SILENT so the Deye falls back to its own CT
W mb_rtu: grid reading fresh again -> meter reports real grid power
```

Zu tun ist dabei nichts am Wechselrichter: **der Alarm verschwindet von selbst**, sobald der Netzzähler zurück ist (gemessen: Register 500 sprang ohne Quittieren auf 2 zurück). Zu suchen ist die Ursache beim Netzzähler — Netzwerk, Gerät, Geräteliste. Wer das alte Verhalten will (unbegrenzt 0 Watt, Deye ohne Alarm), stellt die Überbrückung unter Mod RTU auf *unbegrenzt*.

## Modbus-Brücke (Port 502)

**Die Verbindung kommt gar nicht zustande.**
Zwei verschiedene Fälle, die sich ähnlich anfühlen:

* **Die Brücke ist aus** (Reiter „Mod RTU", Abschnitt TCP-Bridge). Dann lauscht niemand auf dem Port, und die Verbindung wird schon beim Aufbau abgelehnt. In diesem Fall bekommst du auch **keine** Fehlercodes wie `0x0A` zu sehen — dafür müsste die Brücke ja laufen.
* **Das Verbindungslimit ist erreicht.** Die Verbindung geht kurz auf und wird sofort wieder geschlossen. Erlaubt sind zwei gleichzeitige Verbindungen; die Statuszeile zeigt es: `Clients 2/2` heißt voll.

**Antwort kommt, aber mit Fehlercode `0x0A`.**
„Weg nicht verfügbar" — es ist kein Bus zuständig. Der Reihe nach prüfen: Bus angehakt? Bus eingeschaltet? Bus auf **Master** gestellt (ein Slave-Bus wird nie gebrückt)? Passt die Unit-ID? Die Statuszeile listet die tatsächlich gebrückten Busse — bleibt keiner übrig, schreibt sie „kein Bus (Master noetig)". Steht dort dagegen ein Bus und du bekommst trotzdem `0x0A`, dann passt die **Unit-ID** nicht: sie landet nur dann automatisch auf dem Bus, wenn genau ein Bus gebrückt ist.

**Antwort kommt, aber mit Fehlercode `0x0B`.**
„Zielgerät antwortet nicht" — der Weg stand, aber es kam keine brauchbare Antwort zurück. Neben „gar nichts gehört" steckt darunter auch eine kaputte Prüfsumme, eine Antwort von der falschen Slave-ID oder ein Funktionscode, den die Brücke nicht zerlegen kann. Meist ist es dasselbe Problem wie ein offline stehender Master-Bus: Slave-ID, Baudrate, Verkabelung. Erst den [Selbsttest](Modbus-RTU#der-selbsttest) laufen lassen.

**Alles antwortet, aber sehr langsam.**
Normal sind ein paar Dutzend Millisekunden. Einige hundert sind es, wenn die Anfrage ausgerechnet dann eintrifft, während die regelmäßige Akku-Abfrage auf ihre Antwort wartet — das ist normal und geht vorbei. Dauerhaft länger wird es, wenn mehrere Programme gleichzeitig fragen: auf RS485 wird nacheinander gearbeitet, die Anfragen stehen also an. Abhilfe: seltener abfragen oder die Werte über [MQTT](MQTT-und-Home-Assistant) beziehen statt sie einzeln zu pollen.

## Deye-Steuerung

**Der Modus wird gesetzt, der Wechselrichter reagiert nicht.**

1. **Ins Log schauen** — die entscheidenden Register werden nach dem Schreiben zurückgelesen, die Antwort steht also da: `reg143 = 5000 verified` oder `reg143 = 5000: inverter reports 20000 -- NOT applied`. Kurzform in `/api/deye/live` unter `ctrl`: `checked` geprüfte Register, `failed` davon abweichend.
2. Läuft überhaupt ein Bus als **Master**? Ohne Master gibt es keinen Schreibweg.
3. Mit dem [Register-Werkzeug](Web-Mirror#register-werkzeug-deye) nachsehen, ob in den Registern das steht, was dort stehen soll (`142/6` und `166/12`).
4. Modell und Gerätesoftware prüfen. Die Adressen sind an einem SG04LP3 ermittelt und nicht offiziell dokumentiert.

**Nach einem Neustart steht der Akku wieder auf Normal.**
Das ist Absicht. Ein Zwangsmodus lebt im Wechselrichter und überlebt unseren Neustart — das Display käme aber als „Normal" zurück, und dann lädt der Deye unbeaufsichtigt weiter. Die Firmware räumt deshalb auf: findet sie beim Start einen gespeicherten Zwang, schreibt sie ihn ungefähr zehn Sekunden später aktiv auf Normal zurück (`stored mode '…' survived the restart … undoing it`). Ebenso läuft ein Zwang nach zwei Stunden von selbst ab — Restzeit in `/api/deye/live` unter `ctrl.left`. Details: [Deye-Steuerung](Deye-Steuerung#ein-zwang-ist-nur-geborgt).

**Die Zwangsladung lädt mit der falschen Leistung.**
Der Wechselrichter reagiert nicht auf die Wattzahl in Register 126, sondern auf den Ladestrom in Ampere in Register 128. Umgerechnet wird mit `Ampere = Watt ÷ 50`, weil der Batteriestrang etwa 50 Volt hat. Bei einer anderen Batteriespannung stimmt dieser Faktor nicht — dann lädt es entsprechend zu schwach oder zu stark.

**Die Zwangsladung hört zu früh auf.**
Dann stehen die Ziel-Ladezustände in den Registern 166–171 noch niedrig. Der Wechselrichter hat sein Ziel erreicht und stellt ab. Beim Umschalten auf „Laden" werden sie auf 99 % gesetzt — passiert das nicht, ist der Schreibvorgang gescheitert.

**Die Zwangsentladung wird gedrosselt.**
Der SLS-Schutz greift, weil die Einspeisung an die Grenze deines Hausanschlusses stößt. Im Protokoll:

```text
W modbus_tcp: SLS guard: export 22400 W > limit 21735 W (SLS 35A) -- throttle → 18000 W
I modbus_tcp: SLS guard: export 19100 W OK -- restore → 20000 W
```

Dein eingestellter Wunschwert bleibt dabei erhalten und wird wiederhergestellt, sobald die Einspeisung sinkt. Erklärung: [Der SLS-Schutz](Deye-Steuerung#der-sls-schutz).

**Die Anzeige friert beim Umschalten ein.**
Sollte nicht passieren, denn das Schreiben läuft in einem eigenen Programmteil. Wenn doch, wurde irgendwo ein Modbus-Schreibaufruf direkt aus der Bildschirm-Verarbeitung gemacht — das blockiert dann so lange, wie die Leitung braucht.

## Die 15-kW-Geschichte

Das ist der wichtigste Abschnitt auf dieser Seite, auch wenn er nach einer Anekdote klingt.

Diese Anlage hat einmal **15 Kilowatt ins Netz eingespeist**, ohne dass jemand das wollte. Der Ablauf:

1. Der Programmteil, der den Netzzähler abfragt, blieb hängen.
2. Der gemessene Netzwert blieb dadurch auf seinem letzten Stand stehen — er sah aber vollkommen normal aus.
3. Der gefälschte Zähler meldete diesen eingefrorenen Wert brav weiter an den Deye.
4. Der Deye regelte dagegen: er drehte auf, sah keine Veränderung in der Messung, drehte weiter auf, sah weiter keine Veränderung — bis zum Anschlag.

Das Fatale daran: nichts war offensichtlich kaputt. Es lief alles, die Zahlen sahen plausibel aus. **Ein toter Sensor in einem laufenden Regelkreis ist schlimmer als gar kein Sensor**, weil der Regler ihn für gesund hält.

Die Konsequenz steckt heute an drei Stellen im Code:

* Der gefälschte Zähler fragt vor jeder Antwort `modbus_tcp_grid_w_fresh(&w, 12000)` — „gib mir den Netzwert, aber nur, wenn er höchstens 12 Sekunden alt ist."
* Bekommt er ein „nein", meldet er **0 Watt** — nicht den letzten bekannten Wert. Er **antwortet aber weiterhin**, denn Schweigen würde der Deye als Zählerausfall werten und auf seinen eigenen Stromwandler umschalten.
* Nach jedem Speichern der Geräteliste gilt der Netzwert absichtlich als ungültig, bis er wieder frisch gelesen wurde.

> [!WARNING]
> Wenn du an `modbus_rtu.c` oder am Netzpfad in `modbus_tcp.c` arbeitest: **diese Kopplung muss bleiben.** Sie sieht wie eine unnötige Vorsichtsmaßnahme aus, ist aber der Unterschied zwischen einer nützlichen und einer gefährlichen Funktion.

## Nach einem Absturz

`GET /ota` sagt jetzt mehr als „PANIC" — so sah es bei einem absichtlich
herbeigeführten Absturz im Webserver aus:

```json
"reset": "PANIC",
"coredump": { "present": 1, "size": 42916, "task": "httpd",
              "pc": "0x4000e092", "reason": "" }
```

Der Task-Name allein beantwortet oft schon die Frage „wer war es". `reason`
bleibt bei einer echten Ausnahme (Speicherzugriffsfehler und Ähnliches) leer —
gefüllt wird es bei `abort()` und fehlgeschlagenen Zusicherungen, also genau bei
Fällen wie `assert failed: sdio_rx_get_buffer`, der [OTA-Absturz](OTA-und-Recovery)
aus dem September. Für den vollen Stapelspeicher das Abbild holen und mit der
Firmware auflösen, die damals lief (die Build-Nummer steht im Abbild):

```bash
curl -s http://<ip>/coredump -o coredump.bin
python $IDF_PATH/components/espcoredump/espcoredump.py info_corefile     -t raw -c coredump.bin .pio/build/guition-p4/firmware.elf
curl -s "http://<ip>/coredump?erase=1"     # Platz für den nächsten
```

Am Gerät durchgespielt: Absturz → `Save core dump to flash` (der Dump läuft auf
seinem eigenen Stack, 996 von 1880 Byte benutzt) → Neustart → `Found core dump
42916 bytes in flash` → Download → GDB löst `main/ota.c:232` samt Registern auf.
Das Abbild übersteht auch ein Neuflashen der Firmware; nur `?erase=1` räumt es
weg. Ohne die passende `firmware.elf` ist es nicht auflösbar — wer eine
Absturzmeldung ernst nimmt, sichert das ELF zum Build.

> [!NOTE]
> Die Coredump-Partition ist **nach** allen anderen an die Tabelle angehängt, damit kein bestehendes Gerät sein NVS verliert. Eine neue Partitionstabelle kommt aber nur per USB aufs Gerät — ein Display, das ausschließlich über WLAN aktualisiert wurde, hat die Partition nicht und meldet `"coredump":{"present":0}`, auch nach einem Absturz. Einmal `pio run -t upload` über Kabel behebt das dauerhaft.

## Bauen und Flashen

**`BUILD MISMATCH: firmware #146 but filesystem #145`**
Programm und Dateisystem wurden getrennt geflasht und sind aus dem Takt. Lösung: beides in einem Befehl — `pio run -e guition-p4 -t upload -t flashfs`. Über WLAN: beide Dateien schreiben und neu starten.

**`Filesystem build unavailable (asset image not flashed?)`**
Der Dateisystem-Abschnitt ist leer oder unlesbar. Unkritisch, die Firmware läuft weiter. Mit `-t flashfs` beziehungsweise `POST /ota/fs` nachziehen.

**Das Firmware-Update wird abgelehnt.**

* `bad size` — die Datei ist größer als der Speicherabschnitt. Meist die falsche Datei erwischt.
* `image invalid` — beschädigt oder für einen anderen Chip gebaut. Wird erkannt, *bevor* umgeschaltet wird; die alte Version läuft weiter.
* Abbruch ohne Meldung — gab es früher wegen erschöpfter Netzwerkkanäle, dagegen steht heute `CONFIG_LWIP_TCP_MSL=5000` in den Einstellungen.

**Die neue Version läuft, verhält sich aber falsch.**
`POST /ota/rollback` schaltet auf die vorige zurück, oder die Notfallseite unter `/recovery` benutzen. Der alte Stand liegt unangetastet im anderen Speicherabschnitt.

**Die Firmware läuft auf einem Modul nicht.**
Manche ESP32-P4 haben älteres Silizium (ECO2) und verstehen bestimmte Maschinenbefehle nicht. `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` muss gesetzt sein.

## MQTT

**Keine Verbindung.**
Adresse, Port und Zugangsdaten prüfen. Broker, die eine Anmeldung verlangen, trennen sofort wieder, wenn keine Zugangsdaten kommen. Zustand ablesen mit `curl -s http://<ip>/api/live` — `mqtt_en` ist „eingeschaltet", `mqtt_conn` ist „verbunden".

**In Home Assistant erscheint nichts.**
„HA Discovery" muss eingeschaltet sein. Die Beschreibungsnachrichten werden nur beim Verbinden gesendet — also einschalten, speichern, dann verbindet sich der Client neu und schickt sie.

**Nach einem Ausfall stehen alte Werte weiter da.**
Das ist das Retain-Flag: der Broker hält die letzte Nachricht vor. Gewollt, damit Home Assistant nach einem Neustart sofort Werte hat. Wer es nicht mag, schaltet Retain ab. Wichtiger ist das Last-Will: damit erscheint zumindest `offline` auf dem Verfügbarkeits-Topic.

## VPN

**Der Tunnel kommt nicht zustande.**
Der erste Verdächtige ist immer die **Uhr**. WireGuard braucht eine plausible Zeit für seinen Handschlag; ohne gestellte Uhr lehnt der Server ab. Also: NTP eingeschaltet? Server erreichbar? Steht im Reiter „Zeit" ein Datum?

Danach: Schlüssel richtig herum eingetragen (privater Schlüssel des Geräts, öffentlicher des Servers), Endpunkt und Port korrekt, und passt `AllowedIPs` beim Server zur eingestellten Tunnel-Adresse?

**Der Tunnel bricht regelmäßig ab.**
Keepalive auf 25 Sekunden setzen. Ohne regelmäßiges Lebenszeichen wirft der Router den Verbindungseintrag weg, und Antworten von außen finden nicht mehr zurück.

## Einstellungen

**Gespeicherte Werte scheinen verloren.**
Wenn ein Reiter leer erscheint und ein Druck auf „Speichern" die Konfiguration löscht, wurde der Einstellungsbildschirm zu früh im Startvorgang aufgebaut — dann liest er aus noch leeren Programmteilen. Er muss in `main.c` **zuletzt** kommen, siehe [Architektur](Architektur#die-startreihenfolge).

**Absturz kurz nach dem Start der Zweidrahtleitung.**
Die Akku-Schreibtask wurde vor der Zweidrahtleitung gestartet und findet deren Zugriffssperre nicht.

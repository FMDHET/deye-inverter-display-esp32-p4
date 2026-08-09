# Fehlersuche

## Wo man zuerst hinschaut

Bevor man rät, holt man sich Informationen. Es gibt fünf Quellen:

| Quelle | Was man sieht |
| --- | --- |
| **Serieller Monitor** | `pio device monitor -e guition-p4` — alle Meldungen der Firmware mit Zeitstempel. Bei Abstürzen wird sogar die Fehlerstelle im Code aufgelöst. |
| **`GET /ota`** | Version, Build-Nummer, welcher Speicherabschnitt läuft, Laufzeit |
| **`GET /api/live`** | alle Messwerte, MQTT- und Uhr-Zustand |
| **`GET /api/devices`** | pro Gerät: antwortet es, und mit welchen Werten |
| **Kopfzeilen der Menüs** | jeder Reiter zeigt oben laufende Zähler — oft steht die Antwort schon dort |

Ein Tipp zur Laufzeit: tippe auf die Uhr. Steht dort nur eine kurze Zeit, hat das Gerät sich neu gestartet — dann lohnt der Blick in den seriellen Monitor mehr als jede weitere Vermutung.

## Bildschirm

**Bild bleibt schwarz, das Gerät läuft aber.**
Wahrscheinlich der Standby: nach einer eingestellten Zeit ohne Berührung geht die Beleuchtung aus. Einmal antippen. Sonst prüfen, ob die Helligkeit auf 0 % steht.

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
Unser gefälschter Zähler antwortet nicht. Prüfen: ist der Bus eingeschaltet, steht er auf *Slave*, und stimmt die Slave-ID? Der Slave-Zähler im Menükopf muss steigen — wenn nicht, kommen die Fragen des Deye nicht bei uns an.

**Der Deye regelt nicht auf den eingestellten Sollwert.**
Sehr wahrscheinlich meldet die Emulation gerade 0 Watt, weil **kein frischer Netzmesswert** vorliegt. Mögliche Ursachen: kein Gerät mit der Rolle `Netz-Zaehler` eingerichtet, dieses Gerät antwortet nicht, oder die Geräteliste wurde eben gespeichert (dann gilt der Wert bis zum ersten Lesen absichtlich als ungültig).

Das ist **kein Fehler, sondern die eingebaute Sicherung.** Warum, steht gleich unten.

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

1. Läuft überhaupt ein Bus als **Master**? Ohne Master gibt es keinen Schreibweg.
2. Mit dem [Register-Werkzeug](Web-Mirror#register-werkzeug-deye) nachsehen, ob in den Registern das steht, was dort stehen soll (`142/6` und `166/12`).
3. Modell und Gerätesoftware prüfen. Die Adressen sind an einem SG04LP3 ermittelt und nicht offiziell dokumentiert.

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

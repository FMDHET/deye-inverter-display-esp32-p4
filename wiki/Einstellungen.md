# Die Menüs am Gerät

Das Zahnrad oben rechts öffnet die Einstellungen. Links stehen acht Reiter, oben eine Leiste mit „Zurück", dem Titel und rechts einem Knopf, der sich je nach Reiter ändert: „Scan" im WLAN-Reiter, „+ Gerät" bei den Netzwerkgeräten, „Speichern" bei den übrigen.

![Legende des Hauptbildschirms](https://raw.githubusercontent.com/FMDHET/deye-inverter-display-esp32-p4/main/docs/img/dashboard-legend.png)

## WLAN

Verbindungsstatus, Netzsuche und die Liste der gespeicherten Netze. Ausführlich erklärt unter [WLAN und Ersteinrichtung](WLAN-und-Captive-Portal#einrichtung-direkt-am-gerät).

## Display

| Einstellung | Was sie macht |
| --- | --- |
| **Helligkeit** | 0 bis 100 %. Die Hintergrundbeleuchtung wird nicht wirklich gedimmt, sondern sehr schnell ein- und ausgeschaltet (Pulsweitenmodulation). Fürs Auge sieht das aus wie dimmen. |
| **Kontrast** | 0 bis 100 %. Legt einen halbtransparenten grauen Schleier über das Bild — ein Softwaretrick, weil das Panel selbst keinen Kontrastregler hat. 100 % heißt: kein Schleier. |
| **Standby** | Beleuchtung nach einer Weile ohne Berührung ausschalten: Aus / 30 s / 1 / 2 / 5 / 10 min. Eine Berührung weckt wieder auf — und *nur* das: der Aufweck-Tipp wird nicht an den Knopf oder Schieber darunter weitergegeben, sonst würde Aufwecken am linken Rand den Netz-Sollwert verschieben. Ein Tipp im Web-Spiegel wirkt dagegen sofort auf das Bedienelement, weil man dort sieht, wohin man zielt. Das Programm läuft dabei normal weiter, nur das Licht ist aus. |
| **Ausrichtung** | Normal oder um 180° gedreht — für Geräte, die kopfüber montiert sind. |

## Mod TCP — Geräte im Netzwerk

Hier steht die Liste der Geräte, die über das Netzwerk abgefragt werden. „+ Gerät" oben rechts legt einen neuen Eintrag an, Antippen bearbeitet einen bestehenden.

| Feld | Erklärung |
| --- | --- |
| **Name (Anzeige)** | Freier Text, nur zur Wiedererkennung — z. B. „PV Garage" oder „Süd". Erscheint in den Detailfenstern. |
| **aktiv** | Aus bedeutet: wird nicht abgefragt. Praktisch zum Eingrenzen von Problemen, ohne den Eintrag zu löschen. |
| **Hersteller** | Fronius, Deye oder Eltako. Bestimmt, wie das Gerät gelesen wird — **hier passieren die meisten Fehler**, siehe [Modbus-TCP](Modbus-TCP#die-falle-mit-dem-eltako-zähler). |
| **Geräte-Typ** | Die Rolle: Netzzähler, Wechselrichter, Batterie und so weiter. Bestimmt, welchen Kreis auf dem Bildschirm der Wert füttert. |
| **IP-Adresse** | die Adresse im Netzwerk |
| **Port** | fast immer 502, die Standard-Tür für Modbus |
| **Slave-ID** | Die Gerätenummer. Mehrere Geräte dürfen dieselbe IP mit verschiedenen IDs haben — bei Wechselrichtern hinter einem gemeinsamen Datenlogger ist das der Normalfall. |
| **Poll (ms)** | Wie oft gefragt wird. 200 bis 60000, Standard 2000 (also alle zwei Sekunden). |
| **Timeout (ms)** | Wie lange auf Antwort gewartet wird, bevor der Versuch als gescheitert gilt. 100 bis 10000, Standard 500. Bei langsamen Datenloggern hochsetzen. |

Oben steht eine laufende Zeile mit Verbindungszählern und aktuellen Werten — praktisch, um sofort zu sehen, ob ein neu angelegtes Gerät antwortet.

## Mod RTU — die Zweidrahtleitungen

Für jeden der beiden Busse (A hängt an GPIO 52/51, B an GPIO 50/49):

| Feld | Werte |
| --- | --- |
| **Schalter** | Bus ein oder aus |
| **Rolle** | *Master* = wir fragen den Deye. *Slave* = wir geben uns als Stromzähler aus. |
| **Slave-ID** | Als Master: welche Nummer wir ansprechen. Als Slave: auf welche Nummer wir antworten. |
| **Baud** | 4800 / 9600 / 19200 / 38400. Muss mit der Einstellung im Wechselrichter übereinstimmen, üblich ist 9600. |

Darunter der **Selbsttest** — der prüft die eigene Hardware, ohne dass der Wechselrichter beteiligt ist. Sehr nützlich zum Eingrenzen von Verkabelungsfehlern, siehe [Modbus-RTU](Modbus-RTU#der-selbsttest).

| Feld | Werte |
| --- | --- |
| **Bei Zählerausfall 0 W liefern für** | unbegrenzt / 30 s / 60 s / 120 s (Voreinstellung 60 s). Wie lange der emulierte Zähler nach dem Ausfall des echten Netzzählers noch mit „0 Watt" antwortet, bevor er ganz verstummt. Danach erkennt der Deye den Zählerausfall und regelt mit seinem eigenen Stromwandler weiter. Warum das die sichere Reihenfolge ist: [Modbus-RTU](Modbus-RTU#und-jetzt-der-wichtige-teil). |

Ganz unten der Abschnitt **TCP-Bridge**. Damit wird das Display zum Modbus-Gateway: Programme im Netzwerk erreichen über Port 502 die Register des Deye, ohne eigenen RS485-Adapter.

| Feld | Werte |
| --- | --- |
| **Schalter** | Brücke ein oder aus. Standard: aus. |
| **Port** | 502 / 503 / 1502 / 5020. Standard 502. |
| **Erreichbare Busse** | Welche der beiden Leitungen über das Netz erreichbar sind — beide gleichzeitig ist erlaubt. Nur Busse in der Rolle *Master* werden gebrückt. |

Alles Weitere — Unit-ID-Zuordnung, Fehlercodes, Home-Assistant-Beispiel — steht unter [Modbus-Brücke](Modbus-Bridge).

## MQTT

Broker-Adresse, Port, Zugangsdaten, Basistopic und drei Schalter (Retain, HA Discovery, Last Will). Was die Schalter bedeuten, steht unter [MQTT und Home Assistant](MQTT-und-Home-Assistant#die-drei-schalter-erklärt).

## Zeit

Zeitabgleich über das Internet ein- oder ausschalten, Server und Zeitzone. Siehe [Zeit und VPN](Zeit-und-VPN#die-uhr).

## VPN

WireGuard-Tunnel für den Fernzugriff: Schlüssel, Adressen, Gegenstelle. Siehe [Zeit und VPN](Zeit-und-VPN#wireguard-tunnel).

## System

**Netzanschluss (SLS-Schalter)** — hier stellst du ein, wie viele Ampere dein Hauptschalter hat: Deaktiviert / 16 / 20 / 25 / 35 / 50 / 63 A. Darunter zeigt das Gerät die daraus berechnete Grenze, zum Beispiel:

```text
Max. Export: 21,7 kW  (35 A × 3 × 230 V × 90%)
```

Diese Grenze bremst die Zwangsentladung, damit dein Hausanschluss nicht überlastet wird. Erklärung: [Der SLS-Schutz](Deye-Steuerung#der-sls-schutz).

**Web-Zugriff** — ein Passwort für alles, was über das Netz etwas **ändert**:

| geschützt | offen |
| --- | --- |
| Firmware- und Dateisystem-Update, Neustart, Rückfall (`/ota…`) | alle Anzeigeseiten und Messwerte (`/deye`, `/api/live`, `/api/meter`, `/api/deye/live`) |
| Register schreiben (`/deye/write`) | Register lesen (`/deye/read`) |
| Sollwert und Phasenmanipulation (`/api/meter/manip`) | `GET /ota` (Version, Speicher, Neustartgrund) |
| Fernbedienung über den Web-Spiegel (`/touch`, `/key`, `/paste`) | das Spiegelbild selbst |
| Log (`/log`) und Sicherung (`/config`) | |

Leeres Feld = kein Schutz, genau wie vor dieser Funktion. Gesetzt wird das Passwort **nur hier am Gerät** — vor dem Display zu stehen ist der einzige Nachweis, den das Netzwerk nicht fälschen kann. Der Haken auf der Tastatur speichert.

Im Browser fragt danach ein normales Anmeldefenster (der Benutzername ist beliebig, es zählt nur das Passwort), auf der Kommandozeile:

```bash
curl -u :meinPasswort -X POST --data-binary @firmware.bin http://<ip>/ota
```

> [!WARNING]
> Das ist HTTP-Basic ohne Verschlüsselung: es hält einen Irrtum oder einen neugierigen Mitbewohner ab, nicht jemanden, der den Netzverkehr mitliest. Von außen erreichbar sollte das Gerät nur über den [VPN-Tunnel](Zeit-und-VPN) sein. Zwei Wege bleiben ohnehin ungeschützt, weil ihr Protokoll kein Passwort kennt: die **Modbus-Brücke auf Port 502** und **MQTT-Kommandos** — wer den Broker erreicht, kann den Akku umschalten.

**Einstellungen sichern** — `GET /config` liefert alles, was du je eingestellt hast, als eine JSON-Datei: WLAN-Netze samt Passwörtern, MQTT-Zugang, Geräteliste, Zähler-Einstellungen und den **privaten WireGuard-Schlüssel**. Genau der ist der Grund für diese Funktion: er steht nirgendwo sonst, und ein gelöschtes NVS bedeutet, den Tunnel auf beiden Seiten neu einzurichten.

```bash
curl -u :meinPasswort http://<ip>/config -o deye-display-config.json     # sichern
curl -u :meinPasswort -X POST --data-binary @deye-display-config.json      http://<ip>/config                                                  # zurueckspielen
curl -u :meinPasswort -X POST http://<ip>/ota/reboot                     # wirksam werden
```

Die Datei enthält Passwörter im Klartext — entsprechend aufbewahren. Zurückgespielt wird nur, was auch hineingehört: eine Datei ohne `"device": "deye-display"` wird abgelehnt, und ein Datenblock mit unerwarteter Länge (etwa aus einer neueren Firmware) wird übersprungen statt halb angewendet. Die Antwort sagt, wie viele Felder übernommen und wie viele übersprungen wurden.

> [!CAUTION]
> Eine Sicherung eines **anderen** Displays einzuspielen überschreibt auch die WLAN-Liste — danach hängt das Gerät möglicherweise in einem Netz, in dem du es nicht erreichst. Das Notfall-WLAN und der Bildschirm bleiben der Rückweg.

---

## Die Bildschirmtastatur

Textfelder öffnen eine Tastatur mit deutschem Layout, Umlauten und Umschaltung zwischen Groß- und Kleinschreibung (`ABC` / `abc`).

Dass die Umlaute funktionieren, ist übrigens nicht selbstverständlich. Die in LVGL eingebauten Schriftarten sind vorgefertigte Bilder von Buchstaben und enthalten nur den englischen Zeichensatz — kein ä, ö, ü, ß. Deshalb liegt in der Firmware eine echte Schriftdatei (Montserrat), aus der die Buchstaben zur Laufzeit berechnet werden. Für die Symbole (Zahnrad, WLAN-Bögen, Batterie) wird weiter die eingebaute Bildschriftart benutzt, weil die Schriftdatei diese Symbole nicht hat.

Bequemer als jede Bildschirmtastatur: über den [Web-Mirror](Web-Mirror) mit der richtigen Tastatur tippen und aus der Zwischenablage einfügen. Bei WireGuard-Schlüsseln ist das praktisch Pflicht.

## Speichern

Reiter mit einfachen Feldern (Display, Mod RTU, MQTT, Zeit, VPN) speichern über den Knopf „Speichern" oben rechts. Die Geräteliste und die WLAN-Liste speichern jeden Eintrag einzeln beim Bestätigen.

Alles landet im NVS-Speicher und übersteht Neustarts und Firmware-Updates.

> [!IMPORTANT]
> Eine Eigenheit für alle, die am Programm arbeiten: der Einstellungsbildschirm wird beim Start absichtlich **zuletzt** aufgebaut — nach allen anderen Programmteilen. Grund: die Reiter lesen ihre Werte aus den Programmteilen, nicht direkt aus dem Speicher. Baut man sie zu früh auf, sind diese noch leer, die Felder bleiben leer, und ein Druck auf „Speichern" würde die echten Einstellungen mit Leere überschreiben. Wer die Startreihenfolge in `main.c` ändert, muss das im Blick behalten — siehe [Architektur](Architektur#die-startreihenfolge).

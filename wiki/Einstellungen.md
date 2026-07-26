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
| **Standby** | Beleuchtung nach einer Weile ohne Berührung ausschalten: Aus / 30 s / 1 / 2 / 5 / 10 min. Eine Berührung weckt wieder auf. Das Programm läuft dabei normal weiter, nur das Licht ist aus. |
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

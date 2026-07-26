# Einstellungen — Referenz

Erreichbar über das Zahnrad oben rechts. Links eine Leiste mit acht Tabs, oben eine Kopfzeile mit „Zurück", Titel und einer kontextabhängigen Schaltfläche rechts („Scan" im WLAN-Tab, „+ Gerät" im Mod-TCP-Tab, „Speichern" in den übrigen).

![Legende des Hauptbildschirms](https://raw.githubusercontent.com/FMDHET/deye-inverter-display-esp32-p4/main/docs/img/dashboard-legend.png)

## WLAN

Status, Netzsuche, gespeicherte Netze. Ausführlich unter [WLAN und Captive Portal](WLAN-und-Captive-Portal).

## Display

| Einstellung | Wirkung |
| --- | --- |
| **Helligkeit** | Hintergrundbeleuchtung über LEDC-PWM, 0–100 % (Standard 80) |
| **Kontrast** | Software-Überlagerung auf der obersten LVGL-Ebene, 0–100 % (Standard 100 = keine Überlagerung) |
| **Standby** | Beleuchtung nach Ablauf ohne Berührung ausschalten: Aus / 30 s / 1 / 2 / 5 / 10 min. Eine Berührung weckt wieder auf. |
| **Ausrichtung** | Normal oder 180° (umgedreht) — für kopfüber montierte Geräte |

## Mod TCP

Die Geräteliste. „+ Gerät" oben rechts legt einen neuen Eintrag an, ein Antippen bearbeitet einen bestehenden.

| Feld | Bemerkung |
| --- | --- |
| **Name (Anzeige)** | freier Text, erscheint in Popups und JSON (z. B. „PV Garage") |
| **aktiv** | Schalter — deaktivierte Geräte werden nicht gepollt |
| **Hersteller** | Fronius / Deye / Eltako → bestimmt das Registerprofil |
| **Geräte-Typ** | Rolle → bestimmt, welchen Knoten das Gerät füttert |
| **IP-Adresse** | |
| **Port** | Standard 502 |
| **Slave-ID** | Unit-ID; mehrere Geräte dürfen dieselbe IP mit verschiedenen IDs haben |
| **Poll (ms)** | 200 … 60000, Standard 2000 |
| **Timeout (ms)** | 100 … 10000, Standard 500 |

Der Kopf zeigt eine Livezeile: verbundene Geräte, Poll- und Fehlerzähler, aktuelle Werte. Details zu Profilen und Rollen: [Modbus-TCP](Modbus-TCP).

## Mod RTU

Pro Bus (A = UART1/GPIO 52-51, B = UART2/GPIO 50-49):

| Feld | Werte |
| --- | --- |
| **Schalter** | Bus aktiv |
| **Rolle** | Master (Deye lesen) oder Slave (Eastron-Emulation) |
| **Slave-ID** | Master: welche ID abgefragt wird. Slave: auf welche ID geantwortet wird. |
| **Baud** | 4800 / 9600 / 19200 / 38400 |

Darunter der **Selbsttest** — siehe [Modbus-RTU](Modbus-RTU#selbsttest).

## MQTT

Broker, Port, Zugangsdaten, Basistopic sowie die Schalter **Retain**, **HA Discovery** und **Last Will**. Details: [MQTT und Home Assistant](MQTT-und-Home-Assistant).

## Zeit

SNTP an/aus, Server, Zeitzone. Details: [Zeit und VPN](Zeit-und-VPN#sntp-uhr).

## VPN

WireGuard: Schlüssel, Tunnel-Adresse, Endpunkt, Keepalive. Details: [Zeit und VPN](Zeit-und-VPN#wireguard-tunnel).

## System

**Netzanschluss (SLS-Schalter)** — Nennstrom des Hauptschalters, Auswahl: Deaktiviert / 16 / 20 / 25 / 35 / 50 / 63 A. Darunter steht die daraus errechnete Grenze, z. B. `Max. Export: 21,7 kW (35 A × 3 × 230 V × 90 %)`. Siehe [SLS-Export-Schutz](Deye-Steuerung#sls-export-schutz).

---

## Bildschirmtastatur

Textfelder öffnen eine Tastatur mit deutschem Layout, Umlauten und Umschaltung zwischen Groß- und Kleinschreibung (`ABC` / `abc`). Die Umlaute funktionieren, weil die Schriften zur Laufzeit aus der mitgelieferten `montserrat_medium.ttf` gerastert werden (Tiny-TTF) — die eingebauten Bitmap-Schriften von LVGL kennen keine Umlaute. Für die Symbolglyphen (FontAwesome) bleibt die Bitmap-Schrift als Rückfall.

Über den [Web-Mirror](Web-Mirror) lässt sich stattdessen die PC-Tastatur benutzen, inklusive Einfügen aus der Zwischenablage.

## Speichern

Tabs mit einfachen Feldern (Display, Mod RTU, MQTT, Zeit, VPN) speichern über „Speichern" oben rechts. Die Geräteliste im Mod-TCP-Tab und die Netzliste im WLAN-Tab speichern pro Eintrag beim Bestätigen.

Alles liegt in NVS und übersteht Firmware-Updates.

> [!IMPORTANT]
> Der Einstellungsbildschirm wird beim Start bewusst **nach** allen Backend-Starts gebaut. Wurde er vorher gebaut, rendern die Tabs aus leeren Caches — die gespeicherten Werte sahen dann aus, als wären sie verloren, und ein Speichern hätte die echte Konfiguration in NVS überschrieben. Wer die Bootreihenfolge in `main.c` ändert, muss `ui_settings_create()` hinter den `*_start()`-Aufrufen halten.

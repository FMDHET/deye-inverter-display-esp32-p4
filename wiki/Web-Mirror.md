# Web-Mirror und Web-Werkzeuge

Das Gerät bringt zwei HTTP-Server mit:

| Port | Aufgabe |
| --- | --- |
| `:80` | Seiten, Eingaben, JSON-Schnittstellen, OTA, Captive Portal |
| `:81` | ausschließlich der MJPEG-Strom |

Die Trennung ist Absicht: ein langlebiger Bildstrom auf demselben Server würde die Touch-Eingaben blockieren.

## Der Spiegel

`http://<ip>/` zeigt **das echte Panelbild** — keine Nachbildung der UI im Browser:

1. Der Framebuffer wird direkt aus dem Panel gelesen (kein erneutes Rendern).
2. Der **Hardware-JPEG-Encoder** des ESP32-P4 komprimiert das Bild (Qualität 80, YUV420).
3. Die Frames gehen als `multipart/x-mixed-replace` an den Browser, alle 120 ms — also etwa 8 Bilder pro Sekunde. Die Begrenzung hält die Konkurrenz um den LVGL-Lock klein.

Zurück in die andere Richtung geht die Eingabe: Zeiger- und Tastaturereignisse aus dem Browser gehen über ein **zweites LVGL-Eingabegerät** in dieselbe UI. Die Webseite verhält sich damit wie der Touchscreen — inklusive Bildschirmtastatur, physischer Tastatur und Zwischenablage.

| Endpunkt | Zweck |
| --- | --- |
| `GET /touch?x=&y=&down=` | Zeigerereignis einspeisen |
| `GET /key?...` | Tastendruck in das fokussierte Textfeld (ASCII, 8 = Rücktaste) |
| `POST /paste` | Text in das fokussierte Feld einfügen |
| `GET /copy` | Inhalt des fokussierten Feldes auslesen |

Praktischer Nebeneffekt: WireGuard-Schlüssel oder MQTT-Passwörter lassen sich einfügen, statt sie auf einer 4,3″-Bildschirmtastatur einzutippen.

> [!NOTE]
> Während eines OTA-Updates wird der Strom pausiert (`web_mirror_pause`). Das gibt den LVGL-Lock frei, das Panel bleibt statisch und flackert nicht.

## Register-Werkzeug `/deye`

`http://<ip>/deye` greift über den RTU-Master-Bus direkt auf die Holding-Register des Deye zu.

**Register-Probe** — beliebige Adressbereiche lesen (FC03), mit Klartext-Deutung bekannter Register. Ausgegeben werden Adresse, Hex, vorzeichenlos, vorzeichenbehaftet, Name und Interpretation. Nützliche Einstiegspunkte:

| Adresse / Anzahl | Zeigt |
| --- | --- |
| `142` / `6` | Energy Mode, Work Mode, Solar Sell |
| `148` / `12` | TOU-Zeiten und TOU-Leistungen |
| `166` / `12` | TOU-SoC-Sollwerte und Netzlade-Flags |

**Schreiben (FC16)** — einzelnes Register setzen.

**Register-Backup** — einen Adressbereich vollständig auslesen und als CSV speichern (`addr;value;hex;signed;name;interp`, in Excel öffenbar), oder eine CSV wieder einspielen. Beim Import wird immer die Spalte `signed` geschrieben; negative Werte werden korrekt nach U16 umgerechnet.

> [!CAUTION]
> Vor dem Import die Zeilen entfernen, die nicht geschrieben werden sollen — Mess- und Statusregister sind meist nur lesbar. Und generell: erst wissen, was die Adresse tut, dann schreiben. Siehe [Deye-Steuerung](Deye-Steuerung).

## JSON-Schnittstellen

`GET /api/live` — alles, was der Hauptbildschirm zeigt:

```json
{"dev":4,"conn":4,"polls":3377131,"err":68669,
 "pv":6867,"haus":2577,"netz":-685,
 "deye_w":-3547,"deye_soc":91,"byd_w":-58,"byd_soc":80,
 "mqtt_en":1,"mqtt_conn":1,"mqtt_host":"10.0.0.50",
 "ntp":1,"time":"2026-07-26 17:41:37","w":800,"h":480}
```

`GET /api/devices` — Livewerte pro Modbus-TCP-Gerät, siehe [Modbus-TCP](Modbus-TCP#livewerte-abfragen).

`GET /ota` — Version, Build, Slots, Laufzeit, siehe [OTA und Recovery](OTA-und-Recovery).

Alle drei senden `Access-Control-Allow-Origin: *` und lassen sich damit direkt aus eigenen Dashboards abfragen.

## Alle Endpunkte

| Methode | Pfad | Port |
| --- | --- | --- |
| `GET` | `/` — Spiegel-Seite | 80 |
| `GET` | `/` — MJPEG-Strom | 81 |
| `GET` | `/touch`, `/key`, `/copy` | 80 |
| `POST` | `/paste` | 80 |
| `GET` | `/api/live`, `/api/devices` | 80 |
| `GET` | `/deye`, `/deye/read`, `/deye/write` | 80 |
| `GET` | `/ota`, `/recovery` | 80 |
| `POST` | `/ota`, `/ota/fs`, `/ota/reboot`, `/ota/rollback` | 80 |
| `GET` | `/scan` · `POST /connect` | 80 |
| `GET` | `/*` — Captive-Portal-Umleitung im AP-Betrieb | 80 |

> [!CAUTION]
> **Nichts davon ist authentifiziert** — weder der Spiegel, noch das Register-Werkzeug, noch OTA. Wer das Netz erreicht, kann den Wechselrichter umstellen und die Firmware ersetzen. Das Gerät gehört in ein vertrauenswürdiges Netz; für Fernzugriff den [WireGuard-Tunnel](Zeit-und-VPN) nutzen und keine Portweiterleitung einrichten.

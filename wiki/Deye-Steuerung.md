# Deye-Steuerung

> [!CAUTION]
> Diese Seite beschreibt **schreibende** Zugriffe auf einen Wechselrichter, der an einer Netzanlage mit Batteriespeicher hängt. Die Registerbelegungen sind an einem konkreten **Deye SG04LP3** ermittelt worden und können bei anderer Firmware oder anderem Modell abweichen. Vor jedem Schreibversuch mit dem Werkzeug unter [`/deye`](Web-Mirror) prüfen, was an der Adresse tatsächlich steht.

## Die drei Modi

Antippen des Deye-Knotens auf dem Hauptbildschirm öffnet das Popup mit **Normal**, **Laden** und **Entladen** sowie einem Leistungsschieber. Erst „Übernehmen" schreibt.

Dieselben Modi sind über MQTT erreichbar — siehe [MQTT und Home Assistant](MQTT-und-Home-Assistant).

## Register

Geschrieben wird mit **FC16** (Write Multiple Registers), nicht mit FC06 — der Deye nimmt FC06 an diesen Adressen nicht an. Alle Schreibzugriffe laufen über den RTU-Master-Bus.

### Normal

Zurück in den Regelbetrieb „Zero Export to CT":

| Register | Wert | Bedeutung |
| --- | --- | --- |
| 142 | 2 | Work Mode = Zero Export to CT |
| 143 | 20000 | Max Sell Power (W) |
| 126 | 5000 | |
| 127 | 10 | |
| 128 | 40 | |
| 166–171 | 13 | SoC-Sollwerte der sechs Zeitfenster (%) |
| 172–177 | 0 | Netzladen der sechs Zeitfenster aus |

### Laden (Zwangsladung)

Der Wechselrichter reagiert hier **nicht** auf eine Wattangabe, sondern auf den Ladestrom:

| Register | Wert | Bedeutung |
| --- | --- | --- |
| 126 | Leistung in W | wird gesetzt, vom Gerät aber ignoriert |
| 127 | 99 | |
| 128 | Leistung / 50 | **Ladestrom in A**, gerechnet mit etwa 50 V Batteriespannung |
| 166–171 | 99 | SoC-Sollwerte auf 99 % — sonst bricht die Ladung beim erreichten Sollwert ab |
| 172–177 | 1 | Netzladen in allen sechs Zeitfenstern erlaubt |

Die Umrechnung `A = W / 50` ist eine Näherung über die Nennspannung des Batteriestrangs. Bei anderer Systemspannung stimmt der Faktor nicht.

### Entladen (Zwangsentladung)

| Register | Wert | Bedeutung |
| --- | --- | --- |
| 142 | 3 | Work Mode = Selling First |
| 143 | Leistung in W | Max Sell Power |

## Leistungsgrenzen

| | |
| --- | --- |
| Minimum | 1000 W |
| Maximum (Schreibpfad) | 20000 W |
| Maximum (Schieber im Popup) | 22000 W, wird auf 20000 W begrenzt |

## Asynchron schreiben

Die Schreibzugriffe laufen in einer eigenen Task (`deye_ctrl`, Priorität 4, Kern 0), nicht auf der UI-Task. Grund: `modbus_rtu_deye_write()` blockiert, bis der RTU-Master-Bus die Anfrage bedient — auf der LVGL-Task würde das das Touch-Display einfrieren.

Die Task wird per Notify geweckt, dabei gilt immer die **letzte** Anfrage. Wer den Schieber bewegt und mehrfach „Übernehmen" drückt, erzeugt keine Warteschlange.

> [!IMPORTANT]
> `deye_ctrl_start()` muss **nach** `modbus_rtu_start()` aufgerufen werden — die Task braucht den Anfrage-Mutex des RTU-Busses. Umgekehrt lief das Gerät früher in einen Absturz beim Start.

## SLS-Export-Schutz

Bei Zwangsentladung kann die Einspeisung den Hausanschluss überlasten. Der Schutz im Tab „System" begrenzt sie:

```text
maximaler Export  =  SLS-Nennstrom × 3 × 230 V × 0,9
```

| SLS | Grenze |
| --- | --- |
| 16 A | 9,9 kW |
| 20 A | 12,4 kW |
| 25 A | 15,5 kW |
| 35 A | 21,7 kW |
| 50 A | 31,0 kW |
| 63 A | 39,1 kW |

Überschreitet die gemessene Einspeisung diese Grenze, drosselt der Schutz Register 143 (Verkaufsleistung) über `deye_ctrl_set_throttled()`. Wichtig dabei: der **Nutzer-Sollwert bleibt unverändert**. Sinkt die Einspeisung wieder, wird der ursprüngliche Wert automatisch wiederhergestellt.

```text
W modbus_tcp: SLS guard: export 22400 W > limit 21735 W (SLS 35A) -- throttle → 18000 W
I modbus_tcp: SLS guard: export 19100 W OK -- restore → 20000 W
```

Deshalb gibt es zwei Abfragen: `deye_ctrl_get_user_power()` liefert, was der Nutzer eingestellt hat, `deye_ctrl_get_power()` was tatsächlich geschrieben wurde.

> [!WARNING]
> Der Schutz ist eine Software-Hilfe und kein Ersatz für die Schutzeinrichtungen der Anlage. Er greift erst, wenn ein Messwert vorliegt, und er kann eine falsch parametrierte Anlage nicht retten.

## Register selbst untersuchen

Unter `http://<ip>/deye` liegt ein Werkzeug für genau diesen Zweck:

* **Register-Probe** — beliebige Holding-Register lesen (FC03), mit Klartext-Deutung bekannter Adressen. Hilfreiche Bereiche: `148/12` zeigt TOU-Zeiten und -Leistungen, `166/12` die SoC-Sollwerte und Netzlade-Flags, `142/6` Energy Mode, Work Mode und Solar Sell.
* **Einzelnes Register schreiben** (FC16) — mit ausdrücklicher Warnung.
* **Bereich exportieren** — Adressbereich vollständig auslesen und als CSV speichern (Spalten `addr;value;hex;signed;name;interp`), in Excel öffenbar.
* **CSV importieren** — die enthaltenen Zeilen per FC16 zurückschreiben, jeweils die Spalte `signed`.

> [!CAUTION]
> Beim Import vorher die Zeilen entfernen, die **nicht** geschrieben werden sollen. Mess- und Statusregister sind in der Regel nur lesbar; ein Rückschreiben führt zu Fehlern oder unerwartetem Verhalten.

Im Repository liegen dazu die Registerkarten: [`deye-register-map.csv`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/deye-register-map.csv) (kommentiert) und `register tables/Deye_SG04LP3.json`.

## Bekannte Lücke

Die Register für die Zwangsladung sind empirisch ermittelt. Die beiden Referenz-Dumps liegen im Repository und zeigen den Unterschied direkt:

* [`docs/register-dumps/laden-soc100.csv`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/docs/register-dumps/laden-soc100.csv) — Zustand bei aktiver Zwangsladung: TOU-SoC-Sollwerte auf 99 %
* [`docs/register-dumps/entladen-soc13.csv`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/docs/register-dumps/entladen-soc13.csv) — Normalzustand: dieselben Register auf 13 %

Die Kombination aus 126/127/128 und den TOU-Registern funktioniert an diesem Gerät zuverlässig, ist aber keine dokumentierte Herstellerschnittstelle. Wer eine bessere Belegung findet: gern melden.

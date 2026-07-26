# Deye-Steuerung — Akku laden und entladen

> [!CAUTION]
> Diese Seite beschreibt, wie man **in den Wechselrichter hineinschreibt**. Das ist etwas anderes, als ihn nur auszulesen: hier ändert man das Verhalten eines Geräts, das an der Hausinstallation und am öffentlichen Netz hängt.
>
> Die Registeradressen sind an einem konkreten **Deye SG04LP3** durch Ausprobieren gefunden worden — sie stehen in keinem offiziellen Handbuch. Bei einem anderen Modell oder einer anderen Gerätesoftware kann dieselbe Adresse etwas völlig anderes bedeuten. **Vorher immer mit dem [Register-Werkzeug](Web-Mirror#register-werkzeug-deye) nachsehen, was an der Adresse steht.**

## Die drei Betriebsarten

Tippst du auf dem Hauptbildschirm auf den Deye-Kreis, öffnet sich ein Fenster mit drei Knöpfen und einem Schieber für die Leistung:

| Knopf | Was passiert |
| --- | --- |
| **Normal** | Der Wechselrichter macht, was er für richtig hält — der übliche Betrieb. |
| **Laden** | Die Batterie wird zwangsweise geladen, auch aus dem Netz. |
| **Entladen** | Die Batterie wird zwangsweise entladen, notfalls ins Netz. |

Wichtig: es passiert erst etwas, wenn du **„Übernehmen"** drückst. Einen Knopf antippen wählt nur aus.

Wozu man das braucht? Zum Beispiel: Strom ist an der Börse gerade billig oder sogar negativ — dann lohnt es sich, den Akku aus dem Netz zu füllen. Oder umgekehrt: der Preis ist hoch, und man will den Akku verkaufen. Automatisieren lässt sich das über [Home Assistant](MQTT-und-Home-Assistant).

## Welche Register geschrieben werden

Geschrieben wird immer mit **FC16**, dem Befehl für „schreibe mehrere Register" — auch dann, wenn nur ein einzelnes Register geändert wird. Der Deye nimmt den einfacheren Befehl FC06 an diesen Adressen nicht an. Solche Eigenheiten sind bei Modbus-Geräten normal.

### Normal

Zurück in den Regelbetrieb. Alles wird auf Ausgangswerte gesetzt:

| Register | Wert | Bedeutung |
| --- | --- | --- |
| 142 | 2 | Betriebsart „Zero Export to CT" — auf null am Zähler regeln |
| 143 | 20000 | maximale Verkaufsleistung in Watt |
| 126 | 5000 | |
| 127 | 10 | |
| 128 | 40 | |
| 166–171 | 13 | Ziel-Ladezustand der sechs Zeitfenster, in Prozent |
| 172–177 | 0 | Laden aus dem Netz: für alle sechs Zeitfenster aus |

Die „sechs Zeitfenster" sind eine Deye-Eigenheit: man kann den Tag in sechs Abschnitte teilen und für jeden festlegen, bis zu welchem Ladezustand geladen werden soll und ob dafür Netzstrom erlaubt ist. Wir setzen alle sechs gleich, weil wir die Zeitsteuerung nicht benutzen — wir wollen ja *jetzt* etwas.

### Laden

Hier gibt es eine Überraschung, die viel Zeit gekostet hat:

| Register | Wert | Bedeutung |
| --- | --- | --- |
| 126 | Leistung in Watt | wird gesetzt, **aber vom Gerät ignoriert** |
| 127 | 99 | |
| 128 | Leistung ÷ 50 | **der Ladestrom in Ampere** — hierauf reagiert er wirklich |
| 166–171 | 99 | Ziel-Ladezustand auf 99 % |
| 172–177 | 1 | Laden aus dem Netz erlaubt |

Zwei Dinge, die man wissen muss:

**Der Wechselrichter will Ampere, nicht Watt.** Man schreibt eine Wattzahl in Register 126, und nichts passiert. Der Wert, auf den er reagiert, ist der Ladestrom in Ampere in Register 128. Umgerechnet wird mit `Ampere = Watt ÷ 50`, weil der Batteriestrang etwa 50 Volt hat. Das ist eine Näherung — bei einer anderen Batteriespannung stimmt der Faktor nicht, und man lädt mit der falschen Leistung.

**Der Ziel-Ladezustand muss hoch.** Steht in den Registern 166–171 noch 13 %, hört der Wechselrichter bei 13 % einfach auf zu laden — er hat sein Ziel ja erreicht. Deshalb werden sie auf 99 % gesetzt. Und weil aus dem Netz geladen werden soll, müssen zusätzlich die Freigaben in 172–177 auf 1 stehen.

### Entladen

Hier ist es angenehm einfach:

| Register | Wert | Bedeutung |
| --- | --- | --- |
| 142 | 3 | Betriebsart „Selling First" — verkaufen hat Vorrang |
| 143 | Leistung in Watt | maximale Verkaufsleistung |

## Leistungsgrenzen

| | |
| --- | --- |
| Minimum | 1000 W |
| Maximum beim Schreiben | 20000 W |
| Maximum am Schieber | 22000 W (wird auf 20000 begrenzt) |

## Warum das Schreiben in einem eigenen Programmteil läuft

Ein Schreibvorgang auf der Zweidrahtleitung dauert. Die Anfrage muss gesendet werden, das Gerät muss antworten, und wenn gerade eine Abfrage läuft, muss man warten, bis der Bus frei ist. Das können mehrere hundert Millisekunden sein.

Würde man das direkt aus dem Bildschirm-Programmteil machen, wäre die Anzeige in dieser Zeit **eingefroren** — der Touchscreen würde nicht reagieren. Deshalb gibt es eine eigene Task (`deye_ctrl`), die auf dem zweiten Rechenkern läuft und nichts anderes tut, als auf Aufträge zu warten und sie abzuarbeiten.

Nebeneffekt: es entsteht **keine Warteschlange**. Wenn du den Schieber bewegst und dreimal „Übernehmen" drückst, gilt immer nur der letzte Auftrag. Das ist genau richtig — man will nicht, dass drei alte Sollwerte hintereinander abgearbeitet werden.

> [!IMPORTANT]
> Diese Task darf erst gestartet werden, **nachdem** die Zweidrahtleitung läuft — sie braucht deren Zugriffssperre. War die Reihenfolge falsch, stürzte das Gerät beim Start ab.

## Der SLS-Schutz

### Das Problem

Bei Zwangsentladung kann eine Menge Leistung ins Netz gehen: der Akku entlädt, die Sonne scheint zusätzlich, und alles fließt nach draußen. Dein Hausanschluss ist aber für einen bestimmten Strom ausgelegt — begrenzt durch den Hauptschalter, meistens 35 Ampere.

Überlastung in Einspeiserichtung ist genauso ein Problem wie in Bezugsrichtung. Nur merkt man es weniger, weil nichts spürbar ausfällt.

### Die Lösung

Im Menü „System" stellst du ein, wie viel Ampere dein Hauptschalter hat. Daraus rechnet die Firmware eine Grenze:

```text
maximaler Export  =  Ampere  ×  3 Phasen  ×  230 Volt  ×  0,9
```

Der Faktor 0,9 ist ein Sicherheitsabstand: es wird bei 90 Prozent der theoretischen Grenze eingegriffen, nicht erst bei 100.

| Hauptschalter | Grenze |
| --- | --- |
| 16 A | 9,9 kW |
| 20 A | 12,4 kW |
| 25 A | 15,5 kW |
| 35 A | 21,7 kW |
| 50 A | 31,0 kW |
| 63 A | 39,1 kW |

Wird die Grenze überschritten, drosselt die Firmware Register 143 (die Verkaufsleistung), bis die Einspeisung wieder passt.

Der wichtige Teil: **dein eingestellter Wunschwert bleibt unangetastet.** Es gibt zwei getrennte Zahlen — was du wolltest und was gerade tatsächlich geschrieben ist. Sinkt die Einspeisung wieder (weil zum Beispiel eine Wolke kommt oder die Waschmaschine anläuft), wird dein Wunschwert automatisch wiederhergestellt.

Im Protokoll sieht man beides:

```text
W modbus_tcp: SLS guard: export 22400 W > limit 21735 W (SLS 35A) -- throttle → 18000 W
I modbus_tcp: SLS guard: export 19100 W OK -- restore → 20000 W
```

> [!WARNING]
> Das ist **Software**. Sie greift nur, wenn ein Messwert vorliegt, und sie hat womöglich Fehler. Sie ist eine Bequemlichkeit und ausdrücklich **kein Ersatz** für die Schutzeinrichtungen der Anlage. Die richtige Absicherung deines Hausanschlusses ist Sache der Elektroinstallation.

## Register selbst untersuchen

Wenn du eigene Register finden oder eine Vermutung prüfen willst, ist das Werkzeug unter `http://<ip-des-geräts>/deye` genau dafür da.

**Register-Probe** — irgendeinen Adressbereich lesen. Neben dem Rohwert steht bei bekannten Adressen auch eine Deutung in Klartext. Gute Startpunkte:

| Adresse / Anzahl | Zeigt |
| --- | --- |
| `142` / `6` | Betriebsart, Energy Mode, Solar Sell |
| `148` / `12` | die Zeiten und Leistungen der sechs Zeitfenster |
| `166` / `12` | die Ziel-Ladezustände und die Netzlade-Freigaben |

**Ein Register schreiben** — für Versuche mit einzelnen Adressen.

**Bereich sichern und zurückschreiben** — ein Adressbereich wird komplett gelesen und als CSV-Datei gespeichert (Spalten `addr;value;hex;signed;name;interp`), die man in Excel öffnen kann. Dieselbe Datei lässt sich später wieder einspielen.

> [!CAUTION]
> Beim Zurückschreiben **vorher alle Zeilen löschen, die nicht geschrieben werden sollen.** Messwerte und Statusregister sind meist nur lesbar; ein Schreibversuch führt zu Fehlern oder unerwartetem Verhalten. Eine gesicherte Datei enthält immer beides — Einstellungen und Messwerte.

Im Repository liegen als Nachschlagewerk: [`deye-register-map.csv`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/deye-register-map.csv) mit Kommentaren und `register tables/Deye_SG04LP3.json`.

## Was hier noch offen ist

Die Register für das Zwangsladen sind durch Vergleichen gefunden worden: Zustand aufnehmen, im Wechselrichter-Menü etwas umstellen, wieder aufnehmen, Unterschiede ansehen. Die beiden entscheidenden Aufnahmen liegen im Repository:

* [`docs/register-dumps/laden-soc100.csv`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/docs/register-dumps/laden-soc100.csv) — während der Zwangsladung: Ziel-Ladezustände auf 99 %
* [`docs/register-dumps/entladen-soc13.csv`](https://github.com/FMDHET/deye-inverter-display-esp32-p4/blob/main/docs/register-dumps/entladen-soc13.csv) — Normalzustand: dieselben Register auf 13 %

Die Kombination aus 126/127/128 und den Zeitfenster-Registern funktioniert an diesem Gerät zuverlässig. Sie ist aber keine offizielle Schnittstelle, und es ist gut möglich, dass es einen saubereren Weg gibt. Wer einen findet: gerne melden.

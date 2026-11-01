# Host-Tests

```bash
make -C test          # bauen und laufen lassen
make -C test clean
```

Kein ESP-IDF, kein Test-Framework, kein Download: ein Compiler und ein
Makefile. Eine Abhängigkeit, die erst geholt werden muss, bevor der erste Test
läuft, wäre das Gegenteil des Zwecks — und ein Bau über das CMake der Firmware
würde zusätzlich deren Build-Nummer-Ziel anstoßen und den Zähler hochzählen.

## Was hier getestet wird — und was nicht

Getestet wird das, was **rechnet und entscheidet**, ohne Hardware:

| Suite | Deckt ab |
| --- | --- |
| `test_modbus_rtu` | `compute_served()` — was der Wechselrichter zu sehen bekommt: die Frische-Schranke, der Netz-Sollwert, die Phasenmanipulation samt NaN- und Grenzwertabwehr. Dazu `crc16`, `sdm630_response`, `clamp_cfg` und die No-Op-Erkennung in `modbus_rtu_set_cfg()`. Und die Wirkungsgradkurve: Stufengrenzen, Aufsummieren, Speichern und Laden über einen Neustart, Wiederholung nach einem fehlgeschlagenen Schreiben, kein Schreiben ohne Änderung, Verwerfen eines fremden oder beschädigten Datensatzes. |
| `test_webauth` | Das Passwort-Tor: fehlender, falscher, kaputter und richtiger `Authorization`-Kopf, Präfixe, Doppelpunkte im Passwort, Setzen und Löschen, und dass ein fehlgeschlagener Flash-Schreibvorgang das Tor **nicht** scharf stellt. |
| `test_nvs_store` | Der Speicher aller Einstellungen, vor allem `get_blob_prefix()`: kürzerer Datensatz (alte Felder erhalten, neue bleiben 0), **längerer** Datensatz (wird gelesen statt abgelehnt — der Rollback-Fall, den man am Gerät gar nicht auslösen kann, dafür bräuchte es eine zukünftige Firmware), fehlender Schlüssel, die Vorgabewerte, und dass eine beschädigte Partition beim Start gelöscht wird. Dazu, dass die Wirkungsgradkurve bei knapper Partition **nicht** geschrieben wird. |
| `test_sls_guard` | Der SLS-Exportschutz — die einzige Stelle, deren Versagen eine Sicherung ist und keine falsche Zahl: dass die Korrektur von der **angewandten** Leistung ausgeht (und nicht vom Nutzer-Sollwert, was den Regler früher schwingen ließ), dass sie konvergiert statt zu pendeln, die Hysterese und die 60-Sekunden-Haltezeit vor dem Zurückgeben, die Schreibratenbremse und ihre Notfall-Umgehung bei großem Überschuss, das Zurückgehen in Stufen bis exakt auf einen krummen Sollwert, und der Überlauf der Betriebszeit nach 49 Tagen. |
| `test_eff_sweep` | Der Leistungsdurchlauf gegen einen simulierten Wechselrichter: die Ablehnungen (Zwangsmodus aktiv, Deye stumm, unsinnige Angaben, eine Richtung länger als die 2-Stunden-Grenze, SoC schon an der Grenze), ein ganzer Durchlauf (sortierte Stufen, nur Leistungsänderungen innerhalb einer Richtung, Normal-Pause beim Richtungswechsel, Normal am Ende mit dem alten Sollwert, Mittelwerte und Netzzähler-Gegenprobe, Verschleißzähler, die Vorgabe 500 W bis 5 kW in 30-s-Stufen ohne eine Sekunde Verzug je Stufe), und die Ausgänge: Bedienung von außen beendet ihn **ohne** Normal zu schreiben, SoC-Grenze beendet eine Richtung auch beim Einschwingen, Abbruch, stummer Deye, nicht bestätigte Leistung. |
| `test_em_cfg` | Die Einstellungen des Energiemanagers: Speichern und Laden über einen Neustart, jede Bereichsgrenze abgelehnt ohne den gültigen Stand zu ändern, Namen ohne JSON-Störzeichen, der Tibber-Token nur auf ausdrücklichen Wunsch geändert oder gelöscht, Vorgaben für alte Datensätze. |
| `test_price_feed` | Tibber (Zeitzonen-Offsets, ct-Umrechnung, stündliche Antwort verteilt, Fehler und 401), Energy-Charts mit Lücken und gelerntem Aufschlag, Strommix, der Abrufplan mit Rückfall, 429 und 13-Uhr-Regel, Tageswechsel, der 100-Viertelstunden-Tag. |
| `test_pv_forecast` | Azimut-Umrechnung, Auslesen mit Lücken und Zeitversatz, das Leistungsmodell, Tagessummen über die Zeitumstellung, die Kalibrierung (Mittelung, Grenzen, Abdeckungsregel, gemeinsame Messung, Rücksetzen bei geänderter Geometrie, Neustart), Abrufplan. |
| `test_load_profile` | Mittelwert je Viertelstunde, Abdeckung, Werktag/Wochenende, Tageszählung, Prognose mit Lücken, Speichern mit Prüfsumme, abgelehnte Datensätze, NVS-Fehler, beide Umstellungstage, Uhr springt zurück. |
| `test_fronius_storage` | Modbus-Rahmen, Suche nach Model 124, Skalierung, die Entladesperre (schreibt genau drei Register, liest zurück, erneuert die Rückfallzeit, stellt beim Aufheben die Originale her), ein still ignoriertes Schreiben, tote Verbindung, und dass ohne Anforderung nie geschrieben wird. |
| `test_em_planner` | Der Planer in Szenarien (Nachtladen nur so viel wie nötig, Mindestvorteil, Sonne am Morgen, Halten vor Laden, SoC-Grenzen, Horizont) und 60 Zufallsszenarien gegen alle Invarianten, dazu die Laufzeit für 192 Viertelstunden. |
| `test_deye_ctrl` | Die Akku-Steuerung: welche Register jeder Modus schreibt (samt der Umrechnung Watt → Ampere in Register 128), die Rückleseverifikation und ihre drei Fehlerfälle — Bus tot, Rückleser tot, und „der Wechselrichter antwortet ok und behält seinen alten Wert" —, Klemmung und Persistenz, dass die SLS-Drosselung den Nutzer-Sollwert nie anfasst, der 2-Stunden-Zähler ohne Unterlauf, und der Neustart-Pfad samt der Regel, dass der gespeicherte Modus erst nach Bestätigung gelöscht wird. Dazu der Schreibpfad der Drosselung: sie fasst **nur** Register 143 an, eine parallel angeforderte Moduswahl sticht sie trotzdem, und der Verschleißzähler sieht auch die unverifizierten Schreibvorgänge. Und `deye_ctrl_retune()`: beim Entladen nur 143, beim Laden nur 126/128, abgelehnt im Normalbetrieb, ohne die 2-Stunden-Uhr neu zu starten; der Durchlauf darf bis 250 W, das gewöhnliche `apply` bleibt bei 1000 W. |

**Nicht** getestet wird alles, was einen Bus, einen Bildschirm oder ein Netz
braucht: die UART-Tasks, LVGL, WLAN, MQTT, OTA. Ebenso nicht die Schleife von
`deye_ctrl_task()` selbst — sie blockiert auf einer Task-Benachrichtigung;
ihre Bestandteile sind einzeln abgedeckt. Das sind Gerätetests — sie
stehen als Messreihen im [Code-Review](../wiki/Code-Review-2026-09.md), weil man
sie nur am angeschlossenen Display fahren kann.

## Wie es gemacht ist

Die interessanten Funktionen sind `static` — und das ist richtig, niemand von
außen darf sie aufrufen. Eine Suite bindet deshalb die **Quelldatei** ein:

```c
#include "modbus_rtu.c"
```

Der naheliegende Gegenvorschlag wäre, die reine Rechenlogik in eine eigene
Datei zu ziehen. Das wäre schöner — und ein Umbau am laufenden Regelpfad eines
Wechselrichters, der an der Hausinstallation hängt. Erst Tests, dann umbauen;
nicht umgekehrt. Weil jede Suite eine Übersetzungseinheit einbindet, darf sie
**nie** mit einer anderen zusammengelinkt werden: eine Suite, ein Binary.

Die IDF- und FreeRTOS-Attrappen liegen in `fakes/`. Für sie gilt eine Regel:

> Eine Attrappe darf einfach sein, aber nicht **unehrlich**. Wo die echte
> Funktion scheitern kann, muss die Attrappe auch scheitern können, und ein
> Test muss sie scheitern lassen können — sonst sieht die Testsuite nur den
> glücklichen Pfad, und genau der war noch nie kaputt.

Deshalb gibt es `fake_nvs_fail`, `fake_grid_fresh` (die Schranke, deren Ausfall
einmal 15 kW Einspeisung erzeugt hat) und eine Zeit, die stillsteht, bis ein
Test sie bewegt.

## Neue Tests dazuschreiben

1. Suite in `test/` anlegen, `#include "harness.h"`, `#include "fakes.h"` und die
   zu testende `.c`-Datei einbinden.
2. In `test/Makefile` bei `SUITES` eintragen und eine Regel dafür anlegen
   (welche Attrappen sie braucht, entscheidet die Zeile).
3. **Gegenprobe machen.** Vorher committen und `make -C test clean` — beides
   aus Erfahrung: ein `git checkout` in der Mutationsschleife hat einmal
   uncommittete Arbeit mitgerissen, und Apples `/usr/bin/make` (GNU make 3.81)
   vergleicht Zeitstempel **sekundengenau**, übersieht eine Änderung also, die
   in derselben Sekunde wie der letzte Build passiert. Drei Mutationen haben
   deshalb schon einmal fälschlich „bestanden". Auf Linux (make 4.x, Nanosekunden)
   und in CI tritt das nicht auf.

   Ein Test, der nicht fehlschlagen kann, ist wertlos:
   einen Fehler in den Produktivcode einbauen und prüfen, dass genau der
   erwartete Test rot wird. Für die beiden Suiten hier ist das gemacht worden —
   Frische-Schranke entfernt (4 Fehler), NaN-Filter entfernt (2), No-Op-Schutz
   entfernt (1), Längenvergleich im Passwort aufgeweicht (4),
   `WWW-Authenticate` weggelassen (1), Passwort trotz Flash-Fehler übernommen
   (2), Ampere-Umrechnung verdoppelt (2), NVS ohne Bestätigung geräumt (2),
   rückgelesenen Wert nicht verglichen (3), Drosselung fasst den Nutzer-Sollwert
   an (2), gespeicherter Modus beim Start ignoriert (1), Präfix-Regel wieder
   ausgebaut (3), AP-Standardpasswort entfernt (1). Danach `git checkout` —
   der Produktivcode bleibt, wie er war.

   Zuletzt zwölf weitere für den Exportschutz und den Selbsttest: Korrektur
   wieder vom Nutzer-Sollwert (2 Tests), Hysterese entfernt (1), Haltezeit
   entfernt (7), Rückgabe springt statt zu stufen (5), Schreibratenbremse
   entfernt (1), Notfall-Umgehung entfernt (1), Mindestleistung entfernt (1),
   Drosselung schreibt wieder den ganzen Modus (2), Moduswahl zur reinen
   Leistungsänderung degradiert (1), Verschleißzähler übersieht die
   unverifizierten Schreibvorgänge (1), Selbsttest ignoriert die Bus-Rollen
   (2), abgeschalteter Bus zählt trotzdem (1).

   Dabei ist die Sekunden-Falle oben ein zweites Mal zugeschnappt: alle zwölf
   Mutationen meldeten „gefangen", der Kontrolllauf danach war aber rot — die
   Läufe waren teils gegen das Binary der **vorigen** Mutation gefahren. Mit
   `make -C test clean` vor jedem Lauf war das Ergebnis ein anderes: eine
   Mutation überlebte, und sie hatte recht. Die doppelte Klemmung auf die
   Mindestleistung in `sls_guard.h` konnte kein Ergebnis ändern und ist jetzt
   einfach. **Also: Mutationsergebnisse ohne sauberen Neubau sind keine
   Ergebnisse.**

Die erwarteten CRC-Werte in `test_modbus_rtu.c` stammen bewusst **nicht** aus
diesem Code, sondern aus einer unabhängigen Python-Implementierung:

```python
def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc
```

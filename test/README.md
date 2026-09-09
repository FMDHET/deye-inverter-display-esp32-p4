# Host-Tests

```bash
make -C test          # bauen und laufen lassen
make -C test clean
```

Kein PlatformIO, kein Test-Framework, kein Download: ein Compiler und ein
Makefile. Eine Abhängigkeit, die erst geholt werden muss, bevor der erste Test
läuft, wäre das Gegenteil des Zwecks — und `pio test` mit der `native`-Plattform
würde zusätzlich die Vor-Skripte der ESP-Umgebung anstoßen und dabei die
Build-Nummer hochzählen.

## Was hier getestet wird — und was nicht

Getestet wird das, was **rechnet und entscheidet**, ohne Hardware:

| Suite | Deckt ab |
| --- | --- |
| `test_modbus_rtu` | `compute_served()` — was der Wechselrichter zu sehen bekommt: die Frische-Schranke, der Netz-Sollwert, die Phasenmanipulation samt NaN- und Grenzwertabwehr. Dazu `crc16`, `sdm630_response`, `clamp_cfg` und die No-Op-Erkennung in `modbus_rtu_set_cfg()`. |
| `test_webauth` | Das Passwort-Tor: fehlender, falscher, kaputter und richtiger `Authorization`-Kopf, Präfixe, Doppelpunkte im Passwort, Setzen und Löschen, und dass ein fehlgeschlagener Flash-Schreibvorgang das Tor **nicht** scharf stellt. |

**Nicht** getestet wird alles, was einen Bus, einen Bildschirm oder ein Netz
braucht: die UART-Tasks, LVGL, WLAN, MQTT, OTA. Das sind Gerätetests — sie
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
3. **Gegenprobe machen.** Ein Test, der nicht fehlschlagen kann, ist wertlos:
   einen Fehler in den Produktivcode einbauen und prüfen, dass genau der
   erwartete Test rot wird. Für die beiden Suiten hier ist das gemacht worden —
   Frische-Schranke entfernt (4 Fehler), NaN-Filter entfernt (2), No-Op-Schutz
   entfernt (1), Längenvergleich im Passwort aufgeweicht (4),
   `WWW-Authenticate` weggelassen (1), Passwort trotz Flash-Fehler übernommen
   (2). Danach `git checkout` — der Produktivcode bleibt, wie er war.

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

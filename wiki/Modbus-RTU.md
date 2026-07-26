# Modbus-RTU

Zwei RS485-Busse, jeder unabhängig als **Master** oder **Slave** konfigurierbar.

| Rolle | Was das Gerät tut |
| --- | --- |
| **Master** | Fragt den Deye ab: SoC (Register 588) und Akku-Leistung (Register 590, S16, positiv = Entladung). Führt außerdem die Schreibzugriffe der [Deye-Steuerung](Deye-Steuerung) und die Anfragen des Register-Werkzeugs `/deye` aus. |
| **Slave** | Verhält sich als **Eastron SDM630** am Zähler-Port des Deye und beantwortet dessen Abfragen. |

Bus 0 ist UART1 (GPIO 52/51), Bus 1 ist UART2 (GPIO 50/49). Einstellbar sind Rolle, Slave-ID und Baudrate (4800 / 9600 / 19200 / 38400, Standard 9600). Konfiguration liegt in NVS, die Pins sind im Board-Header fest.

## Die Eastron-Emulation

Der Deye regelt anhand seines Zähler-Ports auf „null am Netzübergabepunkt". Hängt dort dieses Gerät, lässt sich der Nullpunkt verschieben:

```text
an den Deye gemeldet  =  echter Netzbezug  −  Sollwert
```

Bei Sollwert `−350 W` sieht der Deye 350 W mehr Einspeisung als real fließt und verschiebt seinen Arbeitspunkt so, dass am Übergabepunkt tatsächlich 350 W eingespeist werden. Eingestellt wird der Sollwert mit dem Schieber links am Hauptbildschirm: −1000 … +1000 W, gerastert auf 50 W, gespeichert in NVS.

### Was emuliert wird

Beantwortet werden FC03 und FC04 (bis 125 Register pro Anfrage). Die Werte sind IEEE-754-float32, aufgeteilt auf Registerpaare — genau wie beim echten SDM630:

| Adresse | Wert |
| --- | --- |
| `0x0000` / `0x0002` / `0x0004` | Spannung L1–L3 → konstant 230 V |
| `0x0006` / `0x0008` / `0x000A` | Strom L1–L3 → \|Leistung je Phase\| / 230 V |
| `0x000C` / `0x000E` / `0x0010` | Leistung L1–L3 → Netzleistung / 3 |
| `0x0034` | Gesamtleistung → Netzleistung |
| `0x0046` | Frequenz → konstant 50 Hz |
| alles andere | 0 |

Vorzeichenkonvention wie beim SDM630: positiv = Bezug aus dem Netz, negativ = Einspeisung. Anfragen mit falscher Slave-ID, falschem Funktionscode oder falscher CRC werden verworfen.

### Die wichtige Sicherung

> [!WARNING]
> Die Emulation liefert den echten Messwert **nur, wenn er frisch ist** (`modbus_tcp_grid_w_fresh()`, maximal 12 s alt). Ist er alt, meldet sie **0 W** — nicht den letzten bekannten Wert.

Warum genau so, und nicht anders:

* **Eingefrorenen Wert melden** wäre ein toter Sensor in einem laufenden Regelkreis. Genau das hat hier einmal eine **Einspeisung von 15 kW** ausgelöst: der Deye regelte gegen eine Zahl, die sich nicht mehr bewegte.
* **Schweigen** wäre auch falsch: der Deye würde „Zähler verloren / nicht verbunden" melden und auf seinen eigenen CT zurückfallen.
* **0 W melden** hält den Zähler in den Augen des Deye lebendig, gibt ihm aber nichts zu regeln — er hält seinen Zustand, bis wieder frische Daten kommen.

Wer an diesem Pfad arbeitet, muss diese Eigenschaft erhalten. Sie ist der Unterschied zwischen einer nützlichen und einer gefährlichen Funktion.

## Der Master-Bus

Der Master pollt den Deye und stellt zwischen den Polls den Bus für Einzelanfragen bereit — Register lesen (FC03, bis 64 Register) und schreiben. Das geschieht bewusst in der Bus-Task und nicht direkt aus dem Aufrufer, damit es keine Konkurrenz auf dem UART gibt.

Gelesene Werte gehen über `modbus_tcp_set_rtu_deye()` in das Energiemodell und haben dort Vorrang gegenüber einem per TCP gelesenen Register 590 — solange sie frisch sind (max. 20 s). Danach verfallen sie, damit der Knoten nicht nachleuchtet, wenn der Bus abgeschaltet wird.

## Selbsttest

Im Tab „Mod RTU" gibt es einen Selbsttest, der die gesamte Kette ohne Wechselrichter prüft. Dazu die beiden Busse gegeneinander verdrahten:

```text
A-TX  →  B-RX
A-RX  →  B-TX
```

Der Master-Bus sendet dann eine FC03-Anfrage an die Slave-ID des anderen Busses, dessen Slave-Task antwortet, und der Master prüft die Antwort. Ergebnis ist `PASS` oder `FAIL` mit Laufzeit in Millisekunden und einer Fehlermeldung.

Das trennt zuverlässig „Verkabelung/Transceiver defekt" von „Wechselrichter antwortet nicht".

## Statusanzeige

Der Kopf des Tabs zeigt pro Bus:

```text
Bus A: Master online   Polls 1384267  Fehler 58   Akku -3073 W / 91 %
Bus B: Slave           Polls 23796193             Netz -106 W
```

* Master: erfolgreiche Lesevorgänge, Fehler, letzte Werte
* Slave: Anzahl beantworteter Anfragen und der aktuell gemeldete Netzwert

Ein langsam mitwachsender Fehlerzähler ist auf einem RS485-Bus normal; ein schnell wachsender deutet auf Abschluss, Masse oder Baudrate hin — siehe [Fehlersuche](Fehlersuche).

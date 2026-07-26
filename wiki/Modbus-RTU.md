# Modbus-RTU — die Zweidrahtleitung zum Deye

Auf dieser Seite geht es um die zwei RS485-Busse, also die direkte Kabelverbindung zum Deye-Wechselrichter. Und um den Kern des ganzen Projekts: den gefälschten Stromzähler.

Wenn dir „Master", „Slave" oder „RS485" nichts sagen: [Glossar](Glossar#wie-geräte-miteinander-reden).

## Zwei Leitungen, zwei Aufgaben

Der Deye hat zwei getrennte Anschlüsse, und wir benutzen beide:

| Bus | Wir sind … | Was wir tun |
| --- | --- | --- |
| **Master** | der Fragende | Wir fragen den Deye nach seinem Ladezustand (Register 588) und der Akkuleistung (Register 590). Über diesen Weg schreiben wir auch Befehle. |
| **Slave** | der Antwortende | Wir geben uns als Stromzähler aus und beantworten die Fragen, die der Deye an seinen Zähler stellt. |

Welcher der beiden Busse welche Rolle hat, ist eine Einstellung und keine Frage der Verkabelung. Einstellbar sind Rolle, Slave-ID und Baudrate (4800 bis 38400, üblich 9600).

## Der gefälschte Zähler — ausführlich

### Was der Deye eigentlich will

Der Deye hat ein einfaches Ziel: **am Hausanschluss soll möglichst genau null Watt fließen.** Kein Strom kaufen, keinen Strom verschenken. Um zu wissen, ob er das erreicht, braucht er eine Messung. Dafür hat er den Zähler-Anschluss.

Er fragt dort ständig: „Wie viel fließt gerade?" Bekommt er „+500 Watt" (also Bezug), erhöht er seine Leistung. Bekommt er „−500 Watt" (also Einspeisung), nimmt er zurück. Ein klassischer Regelkreis.

### Was wir daraus machen

An diesem Anschluss hängt kein echter Zähler, sondern unser Display. Und das antwortet nicht mit dem echten Wert, sondern mit:

```text
Antwort  =  echter Netzwert  −  Wunschwert
```

Der Wunschwert kommt vom Schieber links auf dem Hauptbildschirm: **−1000 bis +1000 Watt**, in 50-Watt-Schritten.

Rechnen wir es durch. Du stellst `−350 W` ein („bitte 350 Watt einspeisen"):

| Situation | echter Wert | wir melden | Deye denkt | Deye macht |
| --- | --- | --- | --- | --- |
| Start | 0 W | +350 W | „ich beziehe Strom!" | dreht auf |
| unterwegs | −200 W | +150 W | „noch nicht genug" | dreht weiter auf |
| Ziel | −350 W | 0 W | „perfekt" | hält |

Am Ende steht der echte Wert bei genau −350 Watt. Der Deye ist zufrieden, weil er glaubt, bei null zu sein.

Das Schöne daran: wir müssen **keine einzige Einstellung im Deye ändern** und keine undokumentierte Schnittstelle benutzen. Wir nutzen nur seinen eigenen Regelkreis — mit einem Offset in der Wahrnehmung.

### Was wir dafür nachbauen müssen

Der Deye erwartet einen Eastron SDM630. Also müssen wir uns wie einer verhalten: die richtigen Register mit den richtigen Zahlenformaten. Wir beantworten FC03 und FC04, maximal 125 Register pro Anfrage:

| Adresse | Was ein SDM630 dort hat | Was wir hineinschreiben |
| --- | --- | --- |
| `0x0000`, `0x0002`, `0x0004` | Spannung L1, L2, L3 | immer 230 V |
| `0x0006`, `0x0008`, `0x000A` | Strom L1, L2, L3 | Leistung je Phase geteilt durch 230 |
| `0x000C`, `0x000E`, `0x0010` | Leistung L1, L2, L3 | Gesamtwert geteilt durch drei |
| `0x0034` | Gesamtleistung | unser Wert |
| `0x0046` | Netzfrequenz | immer 50 Hz |
| alles andere | | 0 |

Die Werte sind Fließkommazahlen, aufgeteilt auf jeweils zwei Register — genau wie beim Original. Vorzeichen wie beim SDM630: positiv = Bezug, negativ = Einspeisung.

Anfragen mit falscher Slave-ID, unbekanntem Funktionscode oder kaputter Prüfsumme werden stillschweigend verworfen — auch das macht ein echtes Gerät so.

### Und jetzt der wichtige Teil

> [!WARNING]
> Der gefälschte Zähler liefert den echten Messwert **nur, wenn dieser frisch ist** — höchstens 12 Sekunden alt. Ist er älter, meldet er **0 Watt**. Nicht den letzten bekannten Wert. Null.

Warum das so gemacht ist, versteht man am besten, wenn man die drei Möglichkeiten vergleicht:

**Möglichkeit 1: den alten Wert weitermelden.** Das klingt harmlos und ist die gefährlichste Variante. Der Deye regelt dann gegen eine Zahl, die sich nicht mehr bewegt. Er dreht auf, sieht keine Reaktion, dreht weiter auf, sieht weiter keine Reaktion — bis er am Anschlag ist. Genau das ist hier passiert: **15 Kilowatt Einspeisung.** Ein toter Sensor in einem laufenden Regelkreis ist schlimmer als gar kein Sensor.

**Möglichkeit 2: einfach nicht antworten.** Klingt vernünftig, ist aber auch schlecht. Der Deye wertet ausbleibende Antworten als „Zähler verloren", meldet einen Fehler und schaltet auf seinen eigenen eingebauten Stromwandler um. Damit ist unsere ganze Steuerung weg, und man bekommt eine Fehlermeldung im Wechselrichter.

**Möglichkeit 3: null melden — so wird es gemacht.** Wir antworten weiter, der Zähler gilt als lebendig, kein Fehler. Aber die Zahl gibt dem Deye nichts zu regeln: „alles im Gleichgewicht, tu nichts." Er hält seinen Zustand, bis wieder echte Daten kommen. Das ist der sichere Ruhezustand.

Wenn du an diesem Teil der Software arbeitest: **diese Eigenschaft muss erhalten bleiben.** Sie ist der Unterschied zwischen einer nützlichen und einer gefährlichen Funktion.

## Der Master-Bus

Der Master fragt den Deye regelmäßig ab. Zwischen diesen Abfragen stellt er den Bus für Einzelaufträge bereit — etwa wenn das [Register-Werkzeug](Web-Mirror#register-werkzeug-deye) im Browser etwas lesen will oder wenn ein [Akku-Befehl](Deye-Steuerung) geschrieben wird.

Wichtig ist, dass das **innerhalb** der Bus-Task passiert und nicht direkt von dort, wo der Auftrag herkommt. Auf einer Zweidrahtleitung darf nur einer sprechen. Würden Bildschirm und Poll-Schleife gleichzeitig senden, gäbe es Datensalat.

Die gelesenen Werte gehen ins Energiemodell und haben dort **Vorrang** vor Werten, die über das Netzwerk gekommen sind — die Kabelverbindung ist die verlässlichere Quelle. Aber auch nur so lange, wie sie frisch sind (höchstens 20 Sekunden). Danach verfallen sie, damit der Kreis nicht ewig einen Wert anzeigt, obwohl der Bus längst abgeschaltet ist.

## Der Selbsttest

Wenn nichts funktioniert, ist die erste Frage immer: liegt es an mir oder am Wechselrichter? Der Selbsttest im Menü „Mod RTU" beantwortet genau das.

Dazu verbindest du die beiden Busse gegeneinander:

```text
Bus A senden     →  Bus B empfangen
Bus A empfangen  ←  Bus B senden
```

Dann schickt der Master-Bus eine Leseanfrage an die Slave-ID des anderen Busses. Der Slave antwortet, der Master prüft die Antwort. Ergebnis: `PASS` oder `FAIL`, mit der gemessenen Laufzeit in Millisekunden.

* **PASS** — Transceiver, Verkabelung zwischen den Bussen und Software sind in Ordnung. Wenn es mit dem Wechselrichter trotzdem nicht geht, liegt es an der Strecke dorthin oder an seinen Einstellungen (Baudrate, Slave-ID, Zählertyp).
* **FAIL** — das Problem ist auf deiner Seite: falsch verdrahtet, Transceiver defekt oder falsche Einstellungen.

## Was der Statuskopf verrät

Oben im Menü „Mod RTU" stehen pro Bus laufende Zähler:

```text
Bus A: Master online   Polls 1384267  Fehler 58   Akku -3073 W / 91 %
Bus B: Slave           Polls 23796193             Netz -106 W
```

So liest man das:

* **Master:** „Polls" sind erfolgreiche Abfragen, dahinter die letzten Werte. Steigt „Polls" nicht, antwortet der Deye nicht.
* **Slave:** „Polls" sind beantwortete Anfragen. Steigt die Zahl nicht, fragt der Deye uns nicht — dann glaubt er, keinen Zähler zu haben.
* **Fehler:** ein langsam mitwachsender Fehlerzähler ist auf einer Zweidrahtleitung völlig normal (elektrische Störungen gibt es immer). Wächst er schnell oder genauso schnell wie „Polls", stimmt etwas mit Abschlusswiderständen, Masse oder Baudrate nicht. Mehr dazu unter [Fehlersuche](Fehlersuche#modbus-rtu).

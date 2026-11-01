# Fronius-Zähler — der Deye als Erzeuger beim Fronius

Ein Fronius-Hybrid-Wechselrichter kennt das Haus nur über seinen eigenen Netzzähler. Was der AC-gekoppelte Deye ins Hausnetz liefert oder zum Laden daraus bezieht, fehlt in seiner Rechnung: Der Hausverbrauch im Fronius und in Solar.web stimmt dann nicht.

Der Fronius kann aber **zusätzliche Zähler über Modbus TCP** abfragen. Das Display gibt sich ihm gegenüber als ein solcher Zähler aus, als Fronius Smart Meter der Kategorie **Erzeuger**, mit den Werten, die der Deye selbst über seine AC-Seite meldet. Der Netzzähler des Fronius bleibt, wie er ist, und damit auch alles, was der Fronius daran regelt und begrenzt.

Dasselbe Verfahren nutzt [The Bridge](https://github.com/FMDHET/Fronius_SM_Reader) für eine AC-gekoppelte Victron-Batterie. Registerabbild und Startsequenz stammen von dort und sind dort am echten Zähler mitgeschnitten.

## Was der Fronius zu sehen bekommt

| | |
| --- | --- |
| Quelle | Deye-Register 627–638 (Spannung, Strom, Wirkleistung je Phase, Frequenz), die der Master-Bus ohnehin alle ~2,5 s liest |
| Vorzeichen | **+ = der Deye bezieht** aus dem Hausnetz (Laden), **− = er liefert** ins Hausnetz (Entladen, eigene PV). So wie ein vor dem Deye eingebauter Zähler. Bei einem Erzeuger-Zähler zählt der Fronius die Einspeisung als Erzeugung |
| Spannung | im Standby meldet der Deye 0 V auf der Wechselrichterseite, dann gilt seine Netzspannung |
| Schein-/Blindleistung | aus U, I und P gerechnet: S = max(U·\|I\|, \|P\|), Q = √(S² − P²) |
| Energiezähler | integriert das Display selbst aus der Leistung (Bezug und Einspeisung getrennt) und speichert sie höchstens alle 15 min. Ein Neustart verliert also bis zu 15 min. Der Stand beginnt bei 0; der Fronius rechnet mit Differenzen |
| Seriennummer | einstellbar: die **Zahl** (Register 20487/20488, die vergleicht und zeigt der Fronius) und der **Text** (20480–20486, genau 7 Buchstaben oder Ziffern). Standard `158028U` bzw. 829686240 — die des **zweiten** Zählers aus The Bridge. Eine Nummer, die der Fronius schon kennt (der Netzzähler hier: 829686239, oder ein Zähler einer Bridge), verwirft er als Doppel. Nach einer Änderung am Fronius neu scannen |
| Keine aktuellen Deye-Werte | älter als 10 s → der Zähler **schweigt**. Der Fronius meldet dann einen Zählerfehler, statt einen eingefrorenen Wert zu verbuchen |

## Einschalten

Auf der Werkbank `/deye`, Reiter **Fronius-Simulator**:

1. **Zähler anbieten** anhaken, Unit-ID wählen (Standard 2), bei Bedarf Seriennummer und Text ändern, **Speichern**. Alles wirkt sofort, ohne Neustart.
2. Der Zähler antwortet auf dem Port der [Modbus-Brücke](Modbus-Bridge) (Standard 502). Er braucht keinen eigenen Socket — Anfragen an seine Unit-ID beantwortet das Display selbst, alle anderen gehen wie bisher an den RS485-Bus. Das gilt auch, wenn die Brücke selbst ausgeschaltet ist.
3. Unit 1 lässt sich nicht wählen, solange ein gebrückter Bus diese Slave-ID hat — sonst wäre der Deye über die Brücke nicht mehr erreichbar.

## Am Fronius einrichten

In der Weboberfläche des Fronius, mit Techniker-Zugang:

1. Zähler → Zähler hinzufügen, Typ **Modbus TCP**
2. IP des Displays, Port 502, Modbus-Adresse = Unit-ID
3. Kategorie **Erzeuger**, dann **Scan**

Danach zeigt der Reiter „Fronius fragt ab“, die Anzahl der Anfragen und die IP des Fronius. Beim Scan sind vier Ausnahmen normal (Register 768, 1706, 40000, 50000 — so antwortet auch der echte Zähler).

## Prüfstand

Am 2026-10-18 vom Rechner aus gegen das Display gespielt: alle 16 verschiedenen Anfragen der mitgeschnittenen Fronius-Startsequenz (`captures/20261004_102928_frames.csv` aus The Bridge) liefern dieselben Exception-Codes, Antwortlängen und Festwerte wie der echte Zähler, bis auf die gewollt andere Seriennummer. Die zyklischen Blöcke 258/286/1024 stimmen mit `/api/deye/live` überein.

**Am Fronius selbst noch nicht geprüft.** Der Symo Hybrid (Datamanager 1.34) bietet den Zählertyp Modbus TCP an; The Bridge hat diesen Weg am 2026-10-04 an einem Fronius im selben Netz mit dem Netzzähler erfolgreich getestet.

## Schnittstelle

| Endpunkt | |
| --- | --- |
| `GET /api/frm` | Einstellung und Status: `en`, `unit`, `sn`, `snt`, `port`, `listening`, `fresh`, Anfragen (`req`, `ok`, `ex`, `silent`), `age` (ms seit der letzten Anfrage), `client`, Leistung `p[3]`/`total`, `imp_kwh`, `exp_kwh` |
| `GET /api/frm/set?en=0\|1&unit=N&sn=Zahl&snt=Text` | einschalten/ausschalten, Unit-ID und Seriennummer setzen; jeder Parameter einzeln. Hinter dem Web-Passwort, wenn eines gesetzt ist |

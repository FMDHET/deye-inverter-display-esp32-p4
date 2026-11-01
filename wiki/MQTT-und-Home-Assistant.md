# MQTT und Home Assistant

## Was MQTT ist

MQTT ist ein Nachrichtendienst für Geräte. Man kann sich ein Schwarzes Brett mit vielen beschrifteten Fächern vorstellen:

* Ein Gerät hängt Nachrichten in ein Fach (das nennt man **veröffentlichen**).
* Andere sagen „ich will alles wissen, was in dieses Fach kommt" (das nennt man **abonnieren**).
* Ein Vermittler in der Mitte, der **Broker**, verteilt alles. Sender und Empfänger kennen sich nicht.

Die Fächer heißen **Topics** und sind wie Ordnerpfade aufgebaut: `deye-display/state`.

Das ist praktisch, weil das Display nicht wissen muss, wer sich für seine Werte interessiert. Es legt sie ins Fach, und wer will, holt sie sich — Home Assistant, ein eigenes Skript, eine Grafana-Anzeige.

Was du brauchst: einen Broker im Netz. Meistens ist das **Mosquitto**, oft schon als Zusatz in Home Assistant installiert.

## Einstellungen

| Feld | Standard | Bedeutung |
| --- | --- | --- |
| Broker | — | IP-Adresse oder Name des Vermittlers |
| Port | 1883 | die Standard-Tür für MQTT |
| Benutzer / Passwort | leer | nur nötig, wenn der Broker es verlangt |
| Basistopic | `deye-display` | der vordere Teil aller Fachnamen |
| Retain | ein | siehe unten |
| HA Discovery | ein | siehe unten |
| Last Will | ein | siehe unten |
| Steuerung erlauben | ein | Aus: MQTT darf nur melden, nicht schalten — siehe unten |

### Die Schalter erklärt

**Retain** („behalten"): Normalerweise vergisst der Broker eine Nachricht, sobald er sie verteilt hat. Wer sich später anmeldet, sieht nichts — bis die nächste Nachricht kommt. Mit Retain merkt sich der Broker die jeweils letzte Nachricht und gibt sie neuen Interessenten sofort. Praktisch: Home Assistant zeigt nach einem Neustart direkt Werte an, statt erst leer zu sein. Nachteil: nach einem Ausfall des Displays stehen die alten Werte noch da, obwohl sie nicht mehr aktuell sind.

**Last Will** („letzter Wille"): Beim Anmelden hinterlegt das Gerät beim Broker eine Nachricht mit der Anweisung „falls ich mich unerwartet nicht mehr melde, schicke das bitte". Es ist dann `offline`. Ohne diesen Mechanismus würden Anzeigen einfach für immer den letzten Wert zeigen, ohne dass jemand merkt, dass die Quelle weg ist.

**HA Discovery**: siehe nächster Abschnitt.

**Steuerung erlauben**: Wer den Broker erreicht, kann sonst den Akku umschalten — MQTT kennt kein zweites Passwort neben dem des Brokers, und das [Passwort für die Web-Zugriffe](Einstellungen#system) hilft hier nicht. Ausgeschaltet passiert dreierlei: das Gerät abonniert die Kommando-Fächer gar nicht erst, ein trotzdem eintreffendes Kommando (etwa eine retained Nachricht aus einer früheren Sitzung) wird abgewiesen und der echte Zustand neu gemeldet, und die zwei Bedienelemente werden in Home Assistant **gelöscht** statt nur nicht mehr angeboten. Das Melden läuft unverändert weiter.

> [!NOTE]
> Der Schalter steht ab Werk auf **ein**, und zwar bewusst: er kam später dazu, und ein Update, das stillschweigend eine laufende Automatisierung abwürgt, schickt einen auf die falsche Fährte. Wer MQTT nur zum Zuschauen benutzt, schaltet ihn aus.

## Die Fächer

| Topic | Richtung | Inhalt |
| --- | --- | --- |
| `<basis>/state` | Gerät → Broker | alle Messwerte als JSON |
| `<basis>/meter/<rolle>` | Gerät → Broker | Zählerstände eines Eltako-Zählers (siehe unten) |
| `<basis>/battery/<akku>` | Gerät → Broker | geladene und entladene Energie je Akku (siehe unten) |
| `<basis>/availability` | Gerät → Broker | `online` oder `offline` |
| `<basis>/deye/mode/set` | Broker → Gerät | `Normal`, `Laden` oder `Entladen` |
| `<basis>/deye/power/set` | Broker → Gerät | Leistung in Watt (1000 bis 20000) |

Die letzten zwei sind der interessante Teil: über die kann man das Gerät **steuern**, nicht nur auslesen.

Der Inhalt von `state`:

```json
{
  "pv_w": 6867,
  "house_w": 2577,
  "grid_w": -685,
  "byd_w": -58,
  "byd_soc": 80,
  "deye_w": -3547,
  "deye_soc": 91,
  "deye_mode": "Normal",
  "deye_power": 5000
}
```

Vorzeichen: `grid_w` positiv heißt Bezug aus dem Netz, negativ heißt Einspeisung. Bei den Akkus heißt positiv entladen, negativ laden. Hier lädt der Deye also mit 3547 Watt und ist zu 91 Prozent voll.

### Zählerstände der Eltako-Zähler

Ein Eltako-Zähler (DSZ15DZ, DSZ16DZ) zählt nicht nur die Leistung, sondern auch die Energie, die insgesamt durch ihn geflossen ist, getrennt nach Richtung. Das Gerät liest beide Zählerstände alle 10 Sekunden (Register 30073 und 30075) und schickt sie in ein eigenes Topic je Zählerrolle:

| Rolle des Zählers | Topic |
| --- | --- |
| Netz-Zähler | `<basis>/meter/grid` |
| Erzeugungs-Zähler | `<basis>/meter/pv_meter` |
| Deye-AC-Zähler | `<basis>/meter/deye_ac` |

```json
{ "import_kwh": 185.64, "export_kwh": 1087.52 }
```

`import_kwh` ist die bezogene Energie (Richtung Haus), `export_kwh` die eingespeiste. Bei einem Zähler mit **Vorzeichen umkehren** sind die beiden vertauscht, damit die Richtung zur Leistung passt. Gibt es mehrere Eltako-Zähler mit derselben Rolle, wird der erste in der Geräteliste gemeldet.

Ein Zählerstand darf nur steigen, und nur so schnell, wie es physikalisch geht. Ein Wert, der rückwärts läuft oder springt, wird verworfen; kommt er dreimal hintereinander, gilt er als echt (Zähler getauscht). Antwortet der Zähler eine Minute lang nicht, schickt das Gerät nichts mehr, statt den alten Stand zu wiederholen.

Auf der Werkbank-API stehen dieselben Werte in `/api/meter` als `imp_kwh` und `exp_kwh`.

### Geladene und entladene Energie der Akkus

Für das Energie-Dashboard braucht Home Assistant je Akku zwei Zählerstände: wie viel Energie insgesamt hineingeladen und wie viel herausgeholt wurde. Das Gerät schickt sie alle 10 Sekunden, ein Topic je Akku:

| Akku | Topic | Woher die Werte kommen |
| --- | --- | --- |
| Deye | `<basis>/battery/deye` | aus den eigenen Zählern des Deye (Register 516/517 und 518/519, 0,1 kWh), alle 30 s über den RS485-Master gelesen |
| BYD | `<basis>/battery/byd` | vom Display aufsummiert, siehe unten |

```json
{ "charge_kwh": 2900.600, "discharge_kwh": 2748.800 }
```

**Der BYD hat keine eigenen Zähler.** Der Fronius Symo Hybrid meldet für seinen Batterieeingang keine Energie (SunSpec-Modell 160 kennzeichnet den Wert als nicht vorhanden, die Solar-API liefert nur Ladezustand, Spannung und Temperatur). Deshalb summiert das Display die BYD-Leistung selbst auf, und zwar dieselbe, die auch in der Energiebilanz steht (Wechselstromseite minus Solarleistung des Hybrid). Die Wandlungsverluste des Wechselrichters stecken damit mit drin. Die Summen liegen im NVS: Sie werden alle 15 Minuten und bei jedem geordneten Neustart (auch beim Update) gespeichert. Nach einem Stromausfall fehlt höchstens die letzte Viertelstunde; rückwärts läuft der Zähler nie. Zählerstände von null an beginnen mit diesem Build.

Wie bei den Eltako-Zählern wird ein Deye-Zählerstand verworfen, der rückwärts läuft oder unmöglich springt.

## Auto-Discovery: Home Assistant richtet sich selbst ein

Normalerweise müsste man in Home Assistant jeden Wert einzeln von Hand konfigurieren: Name, Einheit, Typ, welches Topic, wie man den Wert aus dem JSON herausholt. Für neun Werte ist das eine lange, fehleranfällige Textdatei.

MQTT-Discovery dreht das um: **das Gerät beschreibt sich selbst.** Es schickt nach dem Verbinden Nachrichten an spezielle Topics unter `homeassistant/...`, in denen steht: „ich bin ein Leistungssensor, ich heiße PV Leistung, meine Einheit ist Watt, mein Wert steht in diesem JSON unter dem Schlüssel `pv_w`." Home Assistant hört auf diese Topics und legt alles automatisch an.

Alles erscheint als ein Gerät namens **„Deye Display"**.

Diese Anzeigen entstehen:

| Anzeige | Einheit | Typ |
| --- | --- | --- |
| PV Leistung | W | Leistung |
| Hausverbrauch | W | Leistung |
| Netz | W | Leistung |
| BYD Leistung | W | Leistung |
| BYD SoC | % | Batterie |
| Deye Leistung | W | Leistung |
| Deye SoC | % | Batterie |

Alle sind als Messwerte gekennzeichnet, also direkt für Verläufe und Statistiken brauchbar.

Für jeden Eltako-Zähler kommen zwei Energie-Anzeigen dazu, beim Netz-Zähler **Netz Bezug** und **Netz Einspeisung** (beim Erzeugungs- und Deye-Zähler heißen sie „… Bezug“ und „… Lieferung“). Sie sind in kWh und als stetig steigende Zählerstände gekennzeichnet (`total_increasing`). Genau das verlangt das **Energie-Dashboard** von Home Assistant: unter *Stromnetz* „Netz Bezug“ als Netzbezug und „Netz Einspeisung“ als Rückspeisung eintragen. Wird ein Zähler aus der Geräteliste entfernt, löscht das Gerät seine beiden Anzeigen wieder.

Für jeden Akku, der Werte liefert, kommen **„Deye Akku geladen“ / „Deye Akku entladen“** bzw. **„BYD Akku geladen“ / „BYD Akku entladen“** dazu, ebenfalls in kWh und `total_increasing`. Im Energie-Dashboard unter *Batteriespeicher* je Akku „… geladen“ als „In die Batterie geladene Energie“ und „… entladen“ als „Aus der Batterie entladene Energie“ eintragen. Als Ladezustand dienen die vorhandenen Anzeigen **Deye SoC** und **BYD SoC**. Die Akku-Anzeigen löscht das Gerät nie von selbst: Fehlen die Werte beim Start kurz, soll Home Assistant die Anzeigen nicht verwerfen und neu anlegen.

Und diese zwei Bedienelemente:

| Element | Typ | Details |
| --- | --- | --- |
| **Deye Modus** | Auswahlliste | `Normal`, `Laden`, `Entladen` |
| **Deye Leistung** | Schieber | 1000 bis 20000 W in 100er-Schritten |

Beide zeigen immer den echten aktuellen Zustand, weil sie ihren Wert aus derselben `state`-Nachricht lesen. Stellst du am Display etwas um, springt Home Assistant mit. Und umgekehrt.

## Von außen steuern

Zum Ausprobieren auf der Kommandozeile:

```bash
# Akku mit 6 kW zwangsweise entladen
mosquitto_pub -h broker -t deye-display/deye/power/set -m 6000
mosquitto_pub -h broker -t deye-display/deye/mode/set  -m Entladen

# zurück in den Normalbetrieb
mosquitto_pub -h broker -t deye-display/deye/mode/set  -m Normal
```

Der Weg dahinter ist genau derselbe wie beim Antippen am Gerät: die Firmware schreibt die passenden Register über die Zweidrahtleitung. Auch der [SLS-Schutz](Deye-Steuerung#der-sls-schutz) greift genauso — es gibt keine Hintertür, die ihn umgeht. Und wie jeder Zwangsmodus läuft auch ein per MQTT gesetzter nach zwei Stunden ab und wird von einem Neustart abgeräumt, siehe [Ein Zwang ist nur geborgt](Deye-Steuerung#ein-zwang-ist-nur-geborgt).

Antwortet das Gerät nicht auf die Kommandos, steht im Log `MQTT control is off`: dann ist der Schalter **Steuerung erlauben** aus.

## Beispiel-Automation

Ein typischer Anwendungsfall: Strom ist an der Börse gerade billig oder negativ, also Akku aus dem Netz füllen.

```yaml
automation:
  - alias: "Akku laden bei negativem Strompreis"
    trigger:
      - platform: numeric_state
        entity_id: sensor.strompreis
        below: 0
    action:
      - action: number.set_value
        target:
          entity_id: number.deye_display_deye_leistung
        data:
          value: 10000
      - action: select.select_option
        target:
          entity_id: select.deye_display_deye_modus
        data:
          option: "Laden"
```

Die genauen Entity-IDs vergibt Home Assistant selbst — die richtigen findest du in den Entwicklerwerkzeugen unter „Zustände".

> [!TIP]
> Baue **immer auch den Rückweg** mit ein. Eine Zwangsladung endet nicht von selbst: sie läuft, bis jemand wieder auf `Normal` stellt. Also entweder eine zweite Automation für die Gegenbedingung oder eine Zeitbegrenzung. Sonst lädt der Akku womöglich die ganze Nacht aus dem Netz durch, weil der Preis um zwei Uhr kurz negativ war.

## Läuft es?

Im Menü „MQTT" steht oben, ob die Verbindung besteht und wie viele Nachrichten schon verschickt wurden. Oder über das Netzwerk:

```bash
curl -s http://<ip-des-geräts>/api/live
# {... "mqtt_en":1,"mqtt_conn":1,"mqtt_host":"10.0.0.50", ...}
```

`mqtt_en` heißt „eingeschaltet", `mqtt_conn` heißt „verbunden". Steht das erste auf 1 und das zweite auf 0, stimmt etwas mit Adresse oder Zugangsdaten nicht — siehe [Fehlersuche](Fehlersuche#mqtt).

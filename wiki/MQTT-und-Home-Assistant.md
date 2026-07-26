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

### Die drei Schalter erklärt

**Retain** („behalten"): Normalerweise vergisst der Broker eine Nachricht, sobald er sie verteilt hat. Wer sich später anmeldet, sieht nichts — bis die nächste Nachricht kommt. Mit Retain merkt sich der Broker die jeweils letzte Nachricht und gibt sie neuen Interessenten sofort. Praktisch: Home Assistant zeigt nach einem Neustart direkt Werte an, statt erst leer zu sein. Nachteil: nach einem Ausfall des Displays stehen die alten Werte noch da, obwohl sie nicht mehr aktuell sind.

**Last Will** („letzter Wille"): Beim Anmelden hinterlegt das Gerät beim Broker eine Nachricht mit der Anweisung „falls ich mich unerwartet nicht mehr melde, schicke das bitte". Es ist dann `offline`. Ohne diesen Mechanismus würden Anzeigen einfach für immer den letzten Wert zeigen, ohne dass jemand merkt, dass die Quelle weg ist.

**HA Discovery**: siehe nächster Abschnitt.

## Die Fächer

| Topic | Richtung | Inhalt |
| --- | --- | --- |
| `<basis>/state` | Gerät → Broker | alle Messwerte als JSON |
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

Alle sind als Messwerte gekennzeichnet, also direkt für Verläufe, Statistiken und das Energie-Dashboard von Home Assistant brauchbar.

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

Der Weg dahinter ist genau derselbe wie beim Antippen am Gerät: die Firmware schreibt die passenden Register über die Zweidrahtleitung. Auch der [SLS-Schutz](Deye-Steuerung#der-sls-schutz) greift genauso — es gibt keine Hintertür, die ihn umgeht.

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

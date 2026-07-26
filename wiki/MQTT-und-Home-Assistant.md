# MQTT und Home Assistant

Alle Livewerte gehen auf Wunsch an einen MQTT-Broker — mit optionaler Home-Assistant-Auto-Discovery, Retain-Flag und Last Will. Konfiguriert wird das im Tab „MQTT", gespeichert in NVS.

## Konfiguration

| Feld | Standard | Bemerkung |
| --- | --- | --- |
| Broker (Host/IP) | — | |
| Port | 1883 | |
| Benutzer / Passwort | leer | optional |
| Basistopic | `deye-display` | Präfix aller Topics |
| Retain | ein | State-Publishes mit Retain-Flag |
| HA Discovery | ein | Konfigurations-Topics für Home Assistant |
| Last Will | ein | `offline` auf dem Availability-Topic bei Verbindungsverlust |

## Topics

| Topic | Richtung | Inhalt |
| --- | --- | --- |
| `<base>/state` | Gerät → Broker | JSON mit allen Messwerten und dem Akku-Modus |
| `<base>/availability` | Gerät → Broker | `online` / `offline` |
| `<base>/deye/mode/set` | Broker → Gerät | `Normal` \| `Laden` \| `Entladen` |
| `<base>/deye/power/set` | Broker → Gerät | Leistung in Watt (1000 … 20000) |

Das State-Objekt:

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

Vorzeichen: `grid_w` positiv = Bezug, negativ = Einspeisung. Akku positiv = Entladung, negativ = Ladung.

## Auto-Discovery

Bei aktivierter Discovery veröffentlicht das Gerät nach dem Verbinden die Konfigurations-Topics unter `homeassistant/...`. Alle Entities erscheinen als ein Gerät **„Deye Display"** (Modell `ESP32-P4`).

Sensoren:

| Entity | Einheit | `device_class` |
| --- | --- | --- |
| PV Leistung | W | `power` |
| Hausverbrauch | W | `power` |
| Netz | W | `power` |
| BYD Leistung | W | `power` |
| BYD SoC | % | `battery` |
| Deye Leistung | W | `power` |
| Deye SoC | % | `battery` |

Alle mit `state_class: measurement`, also direkt für Statistiken und das Energie-Dashboard brauchbar.

Steuerelemente:

| Entity | Typ | Details |
| --- | --- | --- |
| **Deye Modus** | `select` | Optionen `Normal`, `Laden`, `Entladen`, Icon `mdi:home-battery` |
| **Deye Leistung** | `number` | 1000 … 20000 W, Schrittweite 100, Darstellung als Schieber, Icon `mdi:battery-charging` |

Beide melden ihren Zustand aus demselben State-JSON zurück — was am Display eingestellt wird, erscheint sofort in Home Assistant und umgekehrt.

## Von außen steuern

```bash
# Zwangsentladung mit 6 kW
mosquitto_pub -h broker -t deye-display/deye/power/set -m 6000
mosquitto_pub -h broker -t deye-display/deye/mode/set  -m Entladen

# zurück in den Regelbetrieb
mosquitto_pub -h broker -t deye-display/deye/mode/set  -m Normal
```

Der Schreibvorgang läuft über denselben Pfad wie die Bedienung am Gerät: [`deye_ctrl_apply()`](Deye-Steuerung) → FC16 über den RTU-Master-Bus. Auch der [SLS-Export-Schutz](Deye-Steuerung#sls-export-schutz) gilt unverändert.

## Beispiel-Automation

```yaml
# Zwangsladung, wenn der Börsenpreis negativ ist
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

Die tatsächlichen Entity-IDs hängen davon ab, wie Home Assistant sie beim Anlegen benennt — bitte in den Entwicklerwerkzeugen nachsehen.

> [!TIP]
> Wer eine Automation baut, die den Modus umschaltet, sollte einen Rückweg nach `Normal` vorsehen (Zeitbegrenzung oder SoC-Bedingung). Eine Zwangsladung bleibt sonst aktiv, bis jemand sie beendet.

## Status prüfen

Im Kopf des MQTT-Tabs steht Verbindungszustand und Anzahl der Publishes. Alternativ über die JSON-Schnittstelle:

```bash
curl -s http://<ip>/api/live
# {... "mqtt_en":1,"mqtt_conn":1,"mqtt_host":"10.0.0.50", ...}
```

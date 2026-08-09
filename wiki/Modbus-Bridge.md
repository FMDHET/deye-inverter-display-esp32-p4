# Die Modbus-Brücke — den Deye vom Netzwerk aus abfragen

Der Deye hängt an einer Zweidrahtleitung. Ein Rechner im WLAN kann damit nichts anfangen — er spricht Netzwerk, nicht RS485. Die Brücke übersetzt zwischen beidem.

Ergebnis: **jedes Modbus-Programm im Heimnetz kann die Register des Deye lesen und schreiben, ohne einen eigenen RS485-Adapter.** Das Display ist der Dolmetscher.

Wenn dir „Modbus", „Register" oder „Slave-ID" nichts sagen: [Glossar](Glossar#wie-geräte-miteinander-reden).

## Nicht zu verwechseln

Es gibt jetzt zwei Netzwerk-Funktionen mit ähnlichem Namen, und sie zeigen in entgegengesetzte Richtungen:

| | Wer fragt? | Wer antwortet? | Seite |
| --- | --- | --- | --- |
| **[Modbus-TCP](Modbus-TCP)** | das Display | fremde Geräte im Netz (Fronius, Eltako …) | das Display holt sich Messwerte |
| **Modbus-Brücke** (diese Seite) | ein Programm im Netz | das Display, das die Frage an den Deye weiterreicht | das Display gibt Werte heraus |

Die erste füttert das Energiebild. Die zweite macht den Deye für andere zugänglich.

## Wie eine Anfrage läuft

```text
Home Assistant                Display                       Deye
      |                          |                            |
      |--- TCP :502 ------------>|                            |
      |    "Register 588, 1x"    |                            |
      |                          |--- RS485 ----------------->|
      |                          |    dieselbe Frage + CRC    |
      |                          |<-- Antwort ----------------|
      |<-- TCP -------------------|                            |
      |    "SoC = 91"            |                            |
```

Technisch passiert dabei wenig: Modbus-TCP und Modbus-RTU tragen **dieselbe Nutzlast**. Der Unterschied steckt nur in der Verpackung. TCP klebt einen 7-Byte-Kopf davor (unter anderem eine laufende Nummer, damit Antworten zugeordnet werden können), RTU hängt stattdessen eine Prüfsumme hinten an. Die Brücke packt um — mehr nicht. Sie versteht die Register nicht und muss es auch nicht.

Ein Detail ist trotzdem kniffelig: **RTU verrät nicht, wie lang eine Antwort wird.** Bei TCP steht die Länge im Kopf. Auf der Zweidrahtleitung muss man sie aus dem Funktionscode ableiten — bei einer Leseantwort steht die Bytezahl im dritten Byte, bei einer Schreibantwort sind es immer acht Byte, bei einer Fehlermeldung fünf. Deshalb kann die Brücke keine reine Byte-Weiterleitung sein.

## Einschalten

Menü **„Mod RTU"**, unterer Abschnitt **„TCP-Bridge"**:

| Einstellung | Bedeutung |
| --- | --- |
| **Schalter** | Brücke an oder aus. Standard: aus. |
| **Port** | 502 (Standard für Modbus-TCP), sonst 503, 1502 oder 5020. |
| **Erreichbare Busse** | Welche der beiden RS485-Leitungen über das Netz erreichbar sind. |

> [!IMPORTANT]
> Gebrückt werden **nur Busse in der Rolle Master**. Ein Slave-Bus ist der, an dem der Deye selbst fragt (der [gefälschte Zähler](Modbus-RTU#der-gefälschte-zähler--ausführlich)). Dort sind wir der Antwortende — würden wir gleichzeitig eigene Fragen hineinsenden, kollidierten die Signale. Ein angehakter Slave-Bus wird deshalb ignoriert. Die Statuszeile listet immer nur die Busse, die wirklich gebrückt sind; bleibt gar keiner übrig, steht dort „kein Bus (Master noetig)".

## Welche Unit-ID landet auf welchem Bus

Jede Modbus-Anfrage trägt eine Geräteadresse mit, die „Unit-ID". Daran entscheidet die Brücke, wohin sie weiterreicht:

1. Passt die Unit-ID zur **Slave-ID eines gebrückten Busses**, geht sie dorthin. So sind beide Leitungen gleichzeitig ansprechbar.
2. Ist nur **ein** Bus gebrückt, geht **jede** Unit-ID an ihn. Damit erreichst du auch weitere Geräte, die an derselben Leitung hängen.
3. Sonst kommt eine Fehlermeldung zurück.

Der Normalfall ist Regel 2: ein Master-Bus zum Deye, gebrückt, fertig — die Unit-ID ist dann egal.

Zwei Busse gleichzeitig funktionieren nur, wenn ihre Slave-IDs **verschieden** sind. Haben beide Geräte die ID 1, kann die Brücke sie nicht auseinanderhalten; ändere dann eine der beiden IDs.

## Wenn etwas nicht geht

Die Brücke antwortet mit den zwei dafür vorgesehenen Modbus-Fehlercodes — die sagen dir sofort, wo du suchen musst:

| Code | Heißt | Ursache |
| --- | --- | --- |
| `0x0A` | „Weg nicht verfügbar" | Kein Bus zuständig: Bus nicht angehakt, Bus abgeschaltet, Bus steht auf Slave, oder die Unit-ID passt zu keiner Regel oben. |
| `0x0B` | „Zielgerät antwortet nicht" | Der Weg stand, aber es kam keine brauchbare Antwort: gar nichts zurück, kaputte Prüfsumme, Antwort von einer anderen Slave-ID, oder ein Funktionscode, dessen Antwortlänge die Brücke nicht bestimmen kann. Meist steckt Slave-ID, Baudrate oder Verkabelung dahinter — der [Selbsttest](Modbus-RTU#der-selbsttest) grenzt das ein. |

> [!NOTE]
> Ist die **Brücke selbst ausgeschaltet**, bekommst du keinen dieser Codes zu sehen: dann lauscht niemand auf dem Port, und die Verbindung scheitert schon beim Aufbau. Fehlercodes gibt es nur von einer laufenden Brücke.

Ein Fehler, den der **Deye selbst** schickt (etwa „Register gibt es nicht"), wird unverändert durchgereicht. Das ist Absicht: ob ein Register existiert, ist die Sache des fragenden Programms, nicht der Brücke.

## Mehrere Programme gleichzeitig

Gleichzeitig offen sein dürfen **zwei** Verbindungen. Das ist fest eingebaut und nicht einstellbar. Wer darüber hinaus anklopft, wird sofort abgewiesen statt hingehalten.

Warum so knapp? Jede Verbindung belegt einen Netzwerkkanal, und davon gibt es im Gerät nur eine feste Anzahl — die teilen sich Webserver, Web-Mirror, MQTT, VPN und die Modbus-Abfragen (siehe [Bauen und Flashen](Bauen-und-Flashen#ein-paar-einstellungen-die-erklärung-brauchen)). Diese Anzahl lässt sich **nicht** einfach erhöhen: der Versuch hat dem WLAN-Baustein den Speicher weggenommen, den er zum Starten braucht, und das Gerät kam gar nicht mehr hoch. Zu viele Modbus-Verbindungen würden also die Weboberfläche aushungern.

Auf der Leitung selbst wird ohnehin **nacheinander** gearbeitet: RS485 lässt nur einen sprechen. Fragen mehrere Programme gleichzeitig, stehen sie kurz an. Für Abfragen im Sekundentakt spielt das keine Rolle.

Verbindungen, die fünf Minuten lang nichts sagen, werden geschlossen — sonst hielte ein abgestürztes Programm seinen Platz für immer.

## Der Deye kommt nicht zu kurz

Die Brücke bekommt den Bus nicht für sich allein. Die regelmäßige Abfrage von Ladezustand und Akkuleistung läuft weiter, und die Steuerbefehle aus dem Menü haben Vorrang. Eine Anfrage aus dem Netz wird **zwischen** diesen Aufgaben eingeschoben, von derselben Stelle im Programm, die auch sonst auf die Leitung schreibt. Auf einer Zweidrahtleitung darf nur einer sprechen — das gilt hier genauso wie beim [Register-Werkzeug](Web-Mirror#register-werkzeug-deye).

Praktisch heißt das: normalerweise wartet eine Anfrage aus dem Netz nur ein paar Dutzend Millisekunden, bis sie an die Reihe kommt. Trifft sie ein, während gerade die Akku-Abfrage unterwegs ist, wartet sie deren Antwort ab — dann werden einige hundert Millisekunden daraus. Und ein Befehl aus dem Menü oder dem [Register-Werkzeug](Web-Mirror#register-werkzeug-deye) wird immer zuerst bedient.

## Home Assistant anbinden

```yaml
modbus:
  - name: deye
    type: tcp
    host: 192.168.1.50      # IP des Displays
    port: 502
    sensors:
      - name: Deye SoC
        slave: 1            # Slave-ID des Deye am RTU-Bus
        address: 588
        input_type: holding
        unit_of_measurement: "%"
      - name: Deye Akkuleistung
        slave: 1
        address: 590
        input_type: holding
        data_type: int16
        unit_of_measurement: W
```

> [!NOTE]
> Für das reine Mitlesen von Messwerten ist [MQTT](MQTT-und-Home-Assistant) der bequemere Weg — das Display schickt die Werte von sich aus, inklusive automatisch angelegter Geräte. Die Brücke ist dann interessant, wenn du an **beliebige** Register willst: zum Ausprobieren, zum Auslesen von Dingen, die nicht über MQTT gehen, oder für Programme, die nur Modbus sprechen.

## Was der Status anzeigt

Unten im Menü „Mod RTU":

```text
Bridge: Port 502   Bus B   Clients 1/2   OK 8412  Fehler 3
```

* **Bus** — welche Leitungen wirklich gebrückt sind. Steht dort „kein Bus (Master noetig)", ist der angehakte Bus abgeschaltet oder auf Slave gestellt.
* **Clients** — offene Verbindungen und das Maximum.
* **OK / Fehler** — beantwortete Anfragen gegen solche, die als Fehlercode zurückgingen. Ein paar Fehler sind auf einer Zweidrahtleitung normal. Wachsen sie genauso schnell wie „OK", stimmt etwas mit Slave-ID, Baudrate oder Verkabelung nicht.

## Sicherheit

> [!WARNING]
> Die Brücke fragt **kein Passwort** ab. Modbus kennt so etwas nicht. Wer den Port erreicht, kann Register des Wechselrichters nicht nur lesen, sondern auch **schreiben**.

Für ein normales Heimnetz hinter einem Router ist das üblich und in Ordnung. Gib den Port aber **nicht** im Router frei. Wenn du von außen zugreifen willst, nimm den [VPN-Tunnel](Zeit-und-VPN) — der ist genau dafür da.

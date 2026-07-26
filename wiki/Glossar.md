# Grundbegriffe

Diese Seite erklärt die Begriffe, die auf den anderen Seiten immer wieder vorkommen. Wenn dir irgendwo ein Wort begegnet, das du nicht kennst — hier steht es.

## Strom und Solaranlage

**PV** heißt Photovoltaik, also Solarstrom. Wenn hier „PV-Leistung" steht, ist gemeint: so viel Strom liefern die Solarmodule gerade.

**Wechselrichter** (englisch *inverter*): Solarmodule liefern Gleichstrom, das Hausnetz braucht Wechselstrom. Der Wechselrichter wandelt das um. Ohne ihn kann man Solarstrom nicht im Haus nutzen.

**Hybrid-Wechselrichter**: ein Wechselrichter, der zusätzlich eine Batterie verwalten kann. Er entscheidet, ob Solarstrom ins Haus geht, in die Batterie geladen oder ins Netz verkauft wird. Der **Deye SG04LP3** in diesem Projekt ist so ein Gerät.

**Speicher / Akku / Batterie** meint hier immer dasselbe: ein großer Hausakku, der Solarstrom für später aufhebt.

**SoC** steht für *State of Charge*, also Ladezustand. `91 %` bedeutet: die Batterie ist zu 91 Prozent voll.

**Netzübergabepunkt**: die Stelle, an der dein Haus am öffentlichen Stromnetz hängt. Dort sitzt der Zähler des Netzbetreibers. Alles, was du nicht selbst verbrauchst, geht hier raus; alles, was du zusätzlich brauchst, kommt hier rein.

**Bezug und Einspeisung**: *Bezug* = Strom kommt aus dem Netz ins Haus. *Einspeisung* = Strom geht aus dem Haus ins Netz. In diesem Projekt gilt durchgehend: **positive Zahl = Bezug, negative Zahl = Einspeisung**. `−350 W` heißt also: 350 Watt gehen gerade ins Netz raus.

**Zähler** (englisch *meter*): ein Messgerät, das sagt, wie viel Strom durch eine Leitung fließt. Dieses Projekt liest mehrere davon.

**MPPT**: die Elektronik im Wechselrichter, die aus den Solarmodulen die maximal mögliche Leistung herausholt. Wenn irgendwo „MPPT-Leistung" steht, ist die reine Gleichstrom-Leistung der Module gemeint — noch ohne Batterieanteil.

**SLS-Schalter**: der Hauptschalter deines Hausanschlusses (*Selektiver Leitungsschutzschalter*), typischerweise mit 35 Ampere. Er begrenzt, wie viel Strom überhaupt durch deinen Anschluss darf — in beide Richtungen.

## Wie Geräte miteinander reden

**Modbus** ist eine sehr alte, sehr einfache Sprache, mit der Industriegeräte miteinander reden. Man kann sich das wie eine Wand mit numerierten Postfächern vorstellen: jedes Gerät hat tausende Fächer, und in jedem steht eine Zahl. Fach 588 enthält zum Beispiel den Ladezustand der Batterie in Prozent.

**Register** ist so ein Postfach. Ein Register enthält immer genau eine 16-Bit-Zahl (0 bis 65535). Größere Werte werden auf zwei Register verteilt.

**Master und Slave**: bei Modbus fragt immer einer, und der andere antwortet. Der Fragende heißt *Master*, der Antwortende *Slave*. Ein Slave sagt nie von sich aus etwas — er wartet, bis er gefragt wird. Dieses Projekt ist mal Master (wenn es den Wechselrichter ausliest) und mal Slave (wenn es sich als Zähler ausgibt).

**Slave-ID** (oder *Unit-ID*): jeder Slave an einer Leitung hat eine Nummer, damit der Master sagen kann, wen er meint. Mehrere Geräte an derselben Adresse mit verschiedenen IDs sind völlig normal.

**Funktionscode** (kurz *FC*): sagt, *was* der Master will.

| Code | Bedeutung |
| --- | --- |
| **FC03** | „Lies mir die Register ab Adresse X vor" (Holding Register) |
| **FC04** | dasselbe für eine andere Registersorte (Input Register) |
| **FC06** | „Schreib diese eine Zahl in Register X" |
| **FC16** | „Schreib diese Zahlen in mehrere Register" |

Beim Deye funktioniert das Schreiben nur mit **FC16**, obwohl oft nur ein einzelnes Register geschrieben wird. FC06 nimmt er an diesen Adressen nicht an — sowas kommt bei Modbus-Geräten häufiger vor.

**Modbus-TCP** heißt: Modbus über das normale Netzwerk (Netzwerkkabel oder WLAN, Adressen wie `192.168.1.30`, meist Port 502). Praktisch, weil man kein extra Kabel ziehen muss.

**Modbus-RTU** heißt: Modbus über eine simple Zweidrahtleitung. Braucht ein extra Kabel, ist dafür unabhängig vom Netzwerk und sehr robust.

**RS485** ist der elektrische Standard hinter Modbus-RTU: zwei verdrillte Adern, genannt **A** und **B**, an denen mehrere Geräte gleichzeitig hängen dürfen. Wie eine alte Telefon-Party­leitung — es darf nur immer einer sprechen. Deshalb braucht man Master/Slave-Regeln.

**Transceiver**: der kleine Baustein, der die Signale des Mikrocontrollers in RS485-Signale übersetzt. „Auto-Direction" bedeutet, dass er selbst merkt, wann gesendet wird — sonst müsste die Software noch eine Umschaltleitung bedienen.

**Baudrate**: wie schnell über die Leitung gesprochen wird, in Zeichen pro Sekunde. 9600 ist der übliche Wert. Beide Seiten müssen dieselbe Baudrate benutzen, sonst versteht keiner den anderen.

**CRC**: eine Prüfsumme am Ende jeder Modbus-Nachricht. Passt sie nicht, war die Nachricht gestört und wird weggeworfen.

**SunSpec** ist ein Zusatz-Standard obendrauf: er legt fest, in welchen Registern welche Werte stehen, damit nicht jeder Hersteller sein eigenes Chaos baut. Fronius hält sich daran, Deye nicht. Ein SunSpec-Gerät hat „Modelle" mit Nummern — Modell 103 ist die Wechselstrom-Leistung, Modell 124 der Batteriespeicher, Modell 160 die Solarmodule.

**Eastron SDM630** ist ein sehr verbreiteter Stromzähler. Der Deye kann mit ihm reden. Genau das nutzt dieses Projekt aus: es **tut so, als wäre es** ein SDM630 — siehe [Modbus-RTU](Modbus-RTU).

## Computerbegriffe

**ESP32-P4** ist der Mikrocontroller, also der kleine Computer, auf dem das Ganze läuft. Kein Betriebssystem wie Windows, sondern nur dieses eine Programm.

**Firmware** ist das Programm auf so einem Mikrocontroller. „Firmware flashen" heißt: das Programm in den Speicher des Chips schreiben.

**ESP-IDF** ist das Entwicklungspaket von Espressif (dem Hersteller des Chips) — Compiler, Bibliotheken, Werkzeuge. **PlatformIO** ist die Umgebung, die das alles startet.

**LVGL** ist die Bibliothek, die Knöpfe, Kreise und Text auf den Bildschirm malt. Sie kümmert sich auch darum, welcher Knopf getroffen wurde, wenn man auf das Display tippt.

**Task** ist ein Programmteil, der gleichzeitig mit anderen läuft. Der Chip hat zwei Rechenkerne, die Tasks wechseln sich ab. Die **Priorität** sagt, wer bei Gleichzeitigkeit Vorrang hat: höhere Zahl gewinnt.

**Lock** (Sperre): wenn zwei Tasks dasselbe anfassen wollen, muss eine warten. Der „LVGL-Lock" stellt sicher, dass immer nur einer am Bildschirminhalt arbeitet.

**NVS** (*Non-Volatile Storage*) ist der kleine Speicherbereich, in dem Einstellungen liegen — vergleichbar mit den gespeicherten Einstellungen deines Routers. Er übersteht Stromausfall und Firmware-Updates.

**SPIFFS** ist ein winziges Dateisystem im Flash-Speicher. Hier liegt eine einzige Datei, die die Build-Nummer enthält.

**Partition**: der Flash-Speicher ist in Abschnitte aufgeteilt — einer für die Einstellungen, zwei für Programmversionen, einer für Dateien.

**OTA** heißt *Over The Air*: Firmware-Update über das Netzwerk statt über USB-Kabel.

**Build-Nummer**: eine Zahl, die bei jedem Bauen hochgezählt wird. So kann man sicher sagen, welche Version gerade läuft.

## Netzwerkbegriffe

**IP-Adresse**: die Hausnummer eines Geräts im Netzwerk, z. B. `192.168.1.42`.

**Port**: gleiche Adresse, verschiedene Türen. Webseiten kommen üblicherweise an Tür 80, Modbus-TCP an Tür 502.

**STA und AP**: Ein WLAN-Chip kann sich in ein bestehendes WLAN einbuchen (*Station*, kurz STA) oder selbst ein WLAN aufspannen (*Access Point*, kurz AP). Dieses Gerät kann beides — und macht Letzteres, wenn es kein bekanntes Netz findet.

**Captive Portal**: das Anmeldefenster, das aufspringt, wenn man sich in ein Hotel- oder Café-WLAN einbucht. Dieses Projekt benutzt denselben Trick für die Ersteinrichtung.

**MQTT** ist ein Nachrichtendienst für Geräte: einer schickt Nachrichten an einen Verteiler (den *Broker*), andere abonnieren sie. Wie ein Schwarzes Brett mit Fächern (*Topics*).

**Home Assistant** ist eine bekannte Software für Hausautomatisierung. Sie kann MQTT lesen und daraus Anzeigen und Schalter bauen.

**Auto-Discovery**: das Gerät beschreibt sich selbst über MQTT, und Home Assistant legt die passenden Anzeigen automatisch an. Man muss nichts von Hand konfigurieren.

**MJPEG**: ein Video, das einfach aus einer schnellen Folge von JPEG-Bildern besteht. Braucht keine komplizierte Videotechnik und läuft in jedem Browser.

**SNTP**: das Protokoll, mit dem Geräte die Uhrzeit aus dem Internet holen.

**VPN / WireGuard**: ein verschlüsselter Tunnel durchs Internet. Damit kommst du von außen an ein Gerät in deinem Heimnetz, ohne es öffentlich erreichbar zu machen. **WireGuard** ist eine moderne, schlanke VPN-Software.

**NAT**: die Technik in deinem Router, die alle Geräte im Haus hinter einer einzigen öffentlichen Adresse versteckt. Deshalb kann man von außen nicht einfach so hineinkommen — und deshalb braucht man einen Tunnel.

## Die Rollen im Energiemodell

Im Programm bekommt jedes angeschlossene Gerät eine **Rolle**. Sie sagt, welchen Wert das Gerät liefert:

| Rolle in der Anzeige | Bedeutung |
| --- | --- |
| `Netz-Zaehler` | der Zähler am Netzübergabepunkt |
| `Erzeugungs-Zaehler` | ein Zähler, der nur die Erzeugung misst |
| `Wechselrichter` | ein Wechselrichter (bei Hybriden zusätzlich Batterie) |
| `Batterie` | ein eigenständiger Speicher |
| `Deye-AC-Zaehler` | ein Zähler direkt vor dem Deye |

> [!NOTE]
> Diese Bezeichnungen erscheinen im Gerät und in der `/api/devices`-Ausgabe genau so — ohne Umlaut, also `Netz-Zaehler` statt `Netz-Zähler`. Das ist ein Überrest aus der Zeit, als die Schriftart auf dem Display noch keine Umlaute konnte. In dieser Doku steht deshalb überall dort, wo der echte Text aus dem Gerät zitiert wird, die Schreibweise ohne Umlaut — im normalen Text natürlich „Zähler".

# Zeit und VPN

## Die Uhr

Oben in der Mitte des Hauptbildschirms stehen Uhrzeit und Datum. Ein Mikrocontroller hat aber keine Uhr, die ohne Strom weiterläuft — nach jedem Neustart wüsste er nicht, welcher Tag ist. Deshalb holt er sich die Zeit aus dem Internet, über ein Protokoll namens **SNTP**.

Einstellungen im Reiter „Zeit":

| Feld | Standard |
| --- | --- |
| NTP aktiv | ein |
| NTP-Server | `pool.ntp.org` |
| Zeitzone | `UTC+1 Berlin` |

`pool.ntp.org` ist ein weltweiter Verbund freiwilliger Zeitserver — man bekommt automatisch einen in der Nähe.

### Warum die Zeitzone mehr ist als eine Zahl

Die Server liefern die Zeit immer in UTC, also der Weltzeit ohne Zeitzonen. Für die Anzeige in Berlin muss man eine Stunde addieren — im Sommer aber zwei. Und die Umstellung ist am letzten Sonntag im März um 2 Uhr beziehungsweise am letzten Sonntag im Oktober um 3 Uhr.

Genau diese Regel steckt in einer kryptischen Zeichenkette:

```text
CET-1CEST,M3.5.0,M10.5.0/3
```

Gelesen: normale Zeit heißt CET und liegt eine Stunde vor UTC, Sommerzeit heißt CEST, sie beginnt im **M**onat 3 in der **5**. Woche am Tag **0** (Sonntag) und endet im Monat 10 in der 5. Woche am Sonntag um 3 Uhr. „5. Woche" ist dabei die Konvention für „die letzte".

Deshalb stellt die Uhr von selbst um, ohne dass man etwas tun muss. Zur Auswahl stehen 13 Zonen:

| Zone | Regel |
| --- | --- |
| UTC−8 Los Angeles | `PST8PDT,M3.2.0,M11.1.0` |
| UTC−5 New York | `EST5EDT,M3.2.0,M11.1.0` |
| UTC−3 São Paulo | `<-03>3` |
| UTC+0 London | `GMT0BST,M3.5.0/1,M10.5.0` |
| UTC+0 UTC | `UTC0` |
| UTC+1 Berlin | `CET-1CEST,M3.5.0,M10.5.0/3` |
| UTC+2 Athen | `EET-2EEST,M3.5.0/3,M10.5.0/4` |
| UTC+3 Moskau | `MSK-3` |
| UTC+4 Dubai | `<+04>-4` |
| UTC+5:30 Neu-Delhi | `IST-5:30` |
| UTC+8 Singapur | `<+08>-8` |
| UTC+9 Tokio | `JST-9` |
| UTC+10 Sydney | `AEST-10AEDT,M10.1.0,M4.1.0/3` |

Wo keine Umstellungsregel steht (Moskau, Dubai, Tokio), gibt es dort keine Sommerzeit.

Oben im Reiter stehen die aktuelle Zeit und ob schon abgeglichen wurde. Solange nicht, bleibt die Anzeige auf dem Hauptbildschirm **leer** — lieber keine Zeit als eine falsche.

Ein Tipp: tippt man auf die Uhr, öffnet sich ein Fenster mit der Laufzeit seit dem letzten Start und einem Knopf für Neustart (mit Rückfrage). Die Laufzeit ist eine gute erste Diagnose — wenn dort nur ein paar Minuten stehen, hat sich das Gerät neu gestartet.

## WireGuard-Tunnel

### Das Problem

Du bist unterwegs und willst nachsehen, was die Solaranlage macht — oder eine neue Firmware aufspielen. Das Gerät hängt aber im Heimnetz hinter dem Router, und der Router versteckt alle Geräte hinter einer einzigen öffentlichen Adresse. Von außen kommt man nicht hinein. Das ist eine Funktion, kein Fehler.

Der naheliegende Ausweg wäre eine **Portweiterleitung** im Router: „alles, was an Tür 80 kommt, an das Display schicken". Bitte nicht. Das Display hat kein Passwort — auf keinem seiner Zugänge. Damit stellt man einen Wechselrichter-Fernsteuerknopf ins offene Internet, und automatische Scanner finden so etwas innerhalb von Stunden.

### Die Lösung

Ein **VPN-Tunnel**: eine verschlüsselte Verbindung, die von innen nach außen aufgebaut wird. Der Router hat nichts dagegen, weil die Verbindung von innen kommt. Wer sich im Tunnel befindet, ist praktisch im Heimnetz — und wer nicht, sieht überhaupt nichts.

**WireGuard** ist eine moderne, sehr schlanke VPN-Software. Statt Benutzernamen und Passwörtern arbeitet sie mit Schlüsselpaaren: jede Seite hat einen privaten Schlüssel (bleibt geheim) und einen öffentlichen (wird der anderen Seite mitgeteilt). Die Implementierung hier ist die eingebundene Komponente [`esp_wireguard`](https://github.com/trombik/esp_wireguard).

### Einrichten

Zuerst ein Schlüsselpaar erzeugen, auf einem Rechner mit installiertem WireGuard:

```bash
wg genkey | tee display.key | wg pubkey > display.pub
```

* `display.key` ist der **private** Schlüssel — der kommt ins Gerät und wird niemandem gezeigt.
* `display.pub` ist der **öffentliche** — der wird beim Server als erlaubte Gegenstelle eingetragen.

Beim Server, zum Beispiel auf einem Raspberry Pi oder im Router (Fritzbox können das inzwischen):

```ini
[Peer]
# Deye-Display
PublicKey = <Inhalt von display.pub>
AllowedIPs = 10.6.0.2/32
```

`AllowedIPs` legt fest, welche Adresse das Display im Tunnel bekommt.

Dann am Gerät im Reiter „VPN":

| Feld | Was dort hinein muss |
| --- | --- |
| WireGuard aktiv | einschalten |
| Privater Schlüssel | Inhalt von `display.key` |
| Public Key | der öffentliche Schlüssel des **Servers** |
| Preshared Key | optional, zusätzlicher gemeinsamer Schlüssel — leer lassen ist in Ordnung |
| Adresse | die Tunnel-Adresse, hier `10.6.0.2` (muss zu `AllowedIPs` passen) |
| Netzmaske | meist `255.255.255.0` |
| Endpunkt | Adresse oder Name des Servers, von außen erreichbar |
| Endpunkt-Port | Standard 51820 |
| Keepalive | 25 Sekunden, wenn das Gerät hinter einem Router steht |

Speichern. Oben im Reiter steht dann, ob die Verbindung zustande gekommen ist.

**Was Keepalive macht:** Der Router merkt sich Verbindungen nur eine Weile. Passiert längere Zeit nichts, wirft er den Eintrag weg, und Antworten von außen finden nicht mehr den Weg zurück. Mit Keepalive schickt das Gerät alle 25 Sekunden ein winziges Lebenszeichen und hält den Weg damit offen.

> [!IMPORTANT]
> **Der Tunnel wird erst aufgebaut, nachdem die Uhr gestellt ist.** Das ist kein Zufall, sondern zwingend: WireGuard baut in seinen Handschlag einen Zeitstempel ein, damit niemand alte, mitgeschnittene Nachrichten wiederverwenden kann. Ist die Uhr des Geräts auf dem Jahr 1970, hält der Server den Handschlag für ungültig und antwortet nicht.
>
> Praktische Folge: **kein NTP, kein VPN.** Wenn der Tunnel nicht zustande kommt, ist die Uhr der erste Verdächtige.

### Wenn der Tunnel steht

Vom Tunnel-Netz aus benutzt man einfach die Tunnel-Adresse:

```bash
curl -s http://10.6.0.2/ota
curl --data-binary @firmware.bin http://10.6.0.2/ota
```

Auch das Display im Browser funktioniert so — nur etwas langsamer, weil das Bild durch den Tunnel muss.

### Eine kuriose technische Notwendigkeit

In den Firmware-Einstellungen steht `CONFIG_ESP_NETIF_BRIDGE_EN=y`. Das klingt nach Netzwerkbrücke und hat mit dem VPN nichts zu tun — es ist ein Trick.

Diese Option schaltet nebenbei eine andere Einstellung um, die dafür sorgt, dass die Netzwerkverwaltung ihre internen Daten an einer anderen Stelle ablegt. Ohne sie legt sie sie genau dort ab, wo WireGuard schon seine eigenen Daten hat. Beide überschreiben sich gegenseitig, und beim nächsten DHCP-Ereignis stürzt das Gerät ab.

Solche Dinge findet man nur durch stundenlanges Debuggen. Deshalb steht ein Kommentar dazu im Code — und deshalb steht es hier.

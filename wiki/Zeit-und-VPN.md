# Zeit und VPN

## SNTP-Uhr

Der Hauptbildschirm zeigt oben in der Mitte Uhrzeit (groß) und Wochentag mit Datum. Beides kommt von einer per SNTP gestellten Systemuhr.

Tab „Zeit":

| Feld | Standard |
| --- | --- |
| NTP aktiv | ein |
| NTP-Server | `pool.ntp.org` |
| Zeitzone | `UTC+1 Berlin` |

Die Zeitzone ist keine reine Stundenverschiebung, sondern eine POSIX-Regel mit Sommerzeitumstellung — die Uhr springt also selbst um. Verfügbar sind 13 Zonen:

| Zone | POSIX-Regel |
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

Der Kopf des Tabs zeigt die aktuelle Zeit und ob bereits synchronisiert wurde. Solange nicht synchronisiert ist, bleibt die Uhr auf dem Hauptbildschirm leer statt eine falsche Zeit zu zeigen.

Antippen der Uhr öffnet ein Popup mit der Laufzeit und einer Schaltfläche zum Neustart (mit Rückfrage).

## WireGuard-Tunnel

Damit ist das Display von außen erreichbar — für den [Web-Mirror](Web-Mirror) und für [OTA-Updates](OTA-und-Recovery) — ohne eine Portweiterleitung einzurichten. Grundlage ist die eingebundene Komponente [`esp_wireguard`](https://github.com/trombik/esp_wireguard) (BSD-3).

Tab „VPN":

| Feld | Inhalt |
| --- | --- |
| WireGuard aktiv | Schalter |
| Privater Schlüssel | dieses Gerät, Base64 (`wg genkey`) |
| Public Key | Gegenstelle / Server (`wg pubkey`) |
| Preshared Key | optional, leer = aus |
| Adresse | Tunnel-IP dieses Geräts, z. B. `10.6.0.2` |
| Netzmaske | z. B. `255.255.255.0` |
| Endpunkt | Host oder IP der Gegenstelle |
| Endpunkt-Port | Standard 51820 |
| Keepalive | Sekunden, 0 = aus |

Der Kopf zeigt, ob der Handschlag zustande gekommen ist.

> [!IMPORTANT]
> Der Tunnel wird erst **nach** der NTP-Synchronisation aufgebaut. Der WireGuard-Handschlag braucht eine plausible Wanduhrzeit; ohne gestellte Uhr scheitert er. Ist SNTP deaktiviert oder der Server nicht erreichbar, kommt kein Tunnel zustande.

### Einrichten

Schlüsselpaar erzeugen (auf einem Rechner mit installiertem WireGuard):

```bash
wg genkey | tee display.key | wg pubkey > display.pub
```

* `display.key` → Feld „Privater Schlüssel" am Gerät
* `display.pub` → beim Server als Peer eintragen

Serverseitig, zum Beispiel:

```ini
[Peer]
# Deye-Display
PublicKey = <Inhalt von display.pub>
AllowedIPs = 10.6.0.2/32
```

Am Gerät den Public Key des Servers, Endpunkt und Port eintragen, Tunnel-IP passend zu `AllowedIPs` setzen, Keepalive auf 25 s, wenn das Gerät hinter NAT steht. Speichern.

> [!TIP]
> Die Schlüssel lassen sich über den Web-Mirror per Zwischenablage einfügen (`POST /paste`) — deutlich angenehmer, als 44 Base64-Zeichen auf der Bildschirmtastatur zu tippen.

### Nach dem Aufbau

Vom Tunnel-Netz aus ist das Gerät unter seiner Tunnel-IP erreichbar:

```bash
curl -s http://10.6.0.2/ota
curl --data-binary @firmware.bin http://10.6.0.2/ota
```

### Eine technische Besonderheit

Damit das funktioniert, ist `CONFIG_ESP_NETIF_BRIDGE_EN=y` gesetzt — nicht wegen Bridging, sondern weil das als Nebeneffekt `LWIP_ESP_NETIF_DATA=1` aktiviert. Ohne das speichert `esp_netif` seinen Handle in `netif->state`, wo das rohe lwIP-Netif von WireGuard schon seine eigene Struktur ablegt; der DHCP-Callback greift dann auf die falschen Daten zu und das Gerät stürzt ab (`Load access fault in esp_netif_internal_dhcpc_cb`).

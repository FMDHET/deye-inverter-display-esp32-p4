# OTA und Recovery

Nach der Erstinstallation über USB wird nur noch über WiFi geflasht.

## Endpunkte

| Methode | Pfad | Wirkung |
| --- | --- | --- |
| `GET` | `/ota` | JSON: laufende Version, Build-Nummer, FS-Build, aktiver und Ziel-Slot, IDF-Version, MAC, Laufzeit |
| `POST` | `/ota` | Firmware-Image (`firmware.bin`) roh im Body → inaktiver Slot, danach Neustart |
| `POST` | `/ota/fs` | Dateisystem-Image (`storage.bin`) roh im Body → `storage`-Partition, **kein** automatischer Neustart |
| `POST` | `/ota/reboot` | Neustart |
| `POST` | `/ota/rollback` | Bootpartition auf den vorigen Slot setzen und neu starten |
| `GET` | `/recovery` | Notfallseite im Browser |

## Der übliche Ablauf

```bash
IP=192.168.1.42

# 1. Was läuft gerade?
curl -s http://$IP/ota
# {"version":"v1.0.57","build":146,"fs_build":146,"running":"ota_1",
#  "target_slot":"ota_0","idf":"5.5.4","mac":"...","uptime":2844443}

# 2. Bauen (zählt die Build-Nummer einmal hoch, erzeugt beide Images)
pio run -e guition-p4

# 3. Firmware schreiben -- das Gerät startet danach selbst neu
curl --data-binary @.pio/build/guition-p4/firmware.bin http://$IP/ota

# 4. Dateisystem schreiben und neu starten
curl --data-binary @.pio/build/guition-p4/storage.bin http://$IP/ota/fs
curl -X POST http://$IP/ota/reboot

# 5. Gegenprüfen: build und fs_build müssen gleich sein
curl -s http://$IP/ota
```

Das Dateisystem wird beim Booten gelesen — deshalb wirkt ein FS-Update erst nach dem Neustart, und deshalb startet `/ota/fs` nicht selbst neu (man will meist ohnehin beides schreiben).

## Zwei Slots

Es gibt `ota_0` und `ota_1`, je 4 MB. Geschrieben wird immer in den **inaktiven** Slot; erst wenn das Image vollständig und gültig ist, wird die Bootpartition umgestellt. Ein abgebrochener Upload lässt die laufende Firmware unangetastet.

`POST /ota/rollback` schaltet zurück auf den anderen Slot — der Weg zurück, wenn eine neue Version bootet, aber sich falsch verhält.

## Kein Flackern beim Flashen

Während des Schreibens:

1. Der MJPEG-Strom wird pausiert (`web_mirror_pause(true)`).
2. Der LVGL-Lock wird geholt und gehalten — die UI zeichnet nicht mehr.
3. Die Hintergrundbeleuchtung geht aus.

Nach Abschluss (oder bei Fehler) wird alles zurückgenommen. Ohne diese Maßnahmen flackert das Panel während des Flash-Zugriffs deutlich sichtbar.

## Die Recovery-Seite

`http://<ip>/recovery` — eine eigenständige Seite, die nichts vom übrigen UI braucht:

* Firmware-Datei auswählen und flashen, mit Fortschrittsbalken
* Dateisystem-Datei auswählen und flashen
* **Rollback** auf den vorigen Slot (mit Rückfrage)
* **Neustart** (mit Rückfrage)

Nützlich, wenn `curl` gerade nicht zur Hand ist oder wenn jemand ohne Entwicklungsumgebung das Gerät wieder gerade ziehen soll.

## Grenzen

> [!CAUTION]
> **Keine Authentifizierung.** Wer das Netz erreicht, kann die Firmware ersetzen. Das Gerät gehört in ein vertrauenswürdiges Netz; für Fernzugriff den [WireGuard-Tunnel](Zeit-und-VPN) nutzen, nicht eine Portweiterleitung.

Weitere Einschränkungen:

* Die Größe wird gegen die Partitionsgröße geprüft, ein zu großes Image abgelehnt (`bad size`).
* Ein ungültiges Firmware-Image scheitert in `esp_ota_end` (`image invalid`) und wird verworfen.
* Beim FS-Update wird die Partition zuerst vollständig gelöscht — ein Abbruch mitten drin lässt ein unbrauchbares Dateisystem zurück. Das ist unkritisch: die Firmware läuft weiter, meldet `Filesystem build unavailable` und man flasht erneut.

#!/usr/bin/env python3
"""Ein Blick auf das laufende Geraet: was stimmt, was nicht.

    python3 scripts/health.py [IP] [--log N]

Fragt die vier Auskunfts-Endpunkte ab und sagt in einer Handvoll Zeilen, ob
das Geraet gesund ist. Gedacht fuer die Tage NACH einer groesseren Aenderung,
an denen man wissen will, ob etwas kippt -- ohne sich jedes Mal zu erinnern,
welche Zahl wo steht und ab wann sie schlecht ist.

Die Schwellen unten sind keine Meinung, sondern das, was dieses Geraet im
Normalbetrieb zeigt. Wer sie anpasst, sollte den Grund danebenschreiben.
"""

import json
import sys
import urllib.error
import urllib.request

IP = "192.168.177.220"
TIMEOUT = 8

# Was auffaellig ist -- und warum genau dieser Wert:
DMA_MIN_WARN = 20 * 1024   # der Absturzfall lag bei einstelligen kB (siehe ota.c)
AGE_WARN_MS = 5000         # der Deye fragt ~8x/s; 5 s Stille ist nie normal
ERR_RATIO_WARN = 0.05      # Lesefehler sind auf RS485/LAN normal, 5 % nicht


def get(ip, path):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=TIMEOUT) as r:
        return json.load(r)


def main():
    args = [a for a in sys.argv[1:]]
    ip = IP
    tail = 0
    while args:
        a = args.pop(0)
        if a == "--log":
            tail = int(args.pop(0)) if args else 40
        else:
            ip = a

    try:
        ota = get(ip, "/ota")
        live = get(ip, "/api/live")
        meter = get(ip, "/api/meter")
        deye = get(ip, "/api/deye/live")
    except (urllib.error.URLError, OSError) as e:
        print(f"nicht erreichbar unter {ip}: {e}")
        return 2

    served = meter["served"]
    up = ota["uptime"]
    notes = []

    print(f"Gerät   {ip}   {ota['version']}  build {ota['build']} / fs {ota['fs_build']}")
    print(f"        Slot {ota['running']} {ota['running_state']}, "
          f"Laufzeit {up // 3600} h {up % 3600 // 60} min, letzter Start: {ota['reset']}")
    print(f"Speicher heap_min {ota['heap_min'] // 1024} kB, "
          f"dma_min {ota['dma_min'] // 1024} kB")
    print(f"Zähler  {'frisch' if served['fresh'] else 'VERALTET'}"
          f"{', STUMM' if served.get('quiet') else ''}, "
          f"{served['req']} Antworten, letzte vor {served['age']} ms, "
          f"Sollwert {served['sp']} W")
    print(f"Geräte  {live['conn']}/{live['dev']} verbunden, "
          f"{live['polls']} Abfragen, {live['err']} Fehler")
    print(f"Werte   PV {live['pv']} W, Haus {live['haus']} W, Netz {live['netz']} W, "
          f"Deye {live['deye_w']} W / {live['deye_soc']} %")
    print(f"Deye    {'online' if deye['online'] else 'OFFLINE'}, "
          f"{deye['blocks']} Registerbloecke, Alter {deye['age']} ms")
    print(f"Sonstiges MQTT {'verbunden' if live['mqtt_conn'] else 'GETRENNT'}, "
          f"Uhr {live['time'] or 'nicht gestellt'}, "
          f"Passwort {'gesetzt' if ota.get('auth') else 'aus'}")

    if ota["reset"] in ("PANIC", "TASK_WDT", "BROWNOUT"):
        notes.append(f"letzter Start war ein {ota['reset']} -- /ota nennt den Coredump")
    cd = ota.get("coredump", {})
    if cd.get("present"):
        notes.append(f"Coredump liegt vor: Task {cd.get('task')}, PC {cd.get('pc')} "
                     f"-- abholen mit GET /coredump, danach ?erase=1")
    if ota["dma_min"] < DMA_MIN_WARN:
        notes.append(f"dma_min bei {ota['dma_min'] // 1024} kB -- OTA wird riskant, "
                     f"etwas belegt internen DMA-Speicher")
    if served["age"] > AGE_WARN_MS and served["req"] > 0:
        notes.append(f"der Deye hat seit {served['age'] // 1000} s nichts gefragt "
                     f"-- Zähler-Bus prüfen, notfalls Gerät stromlos machen")
    if not served["fresh"]:
        notes.append("kein frischer Netzmesswert -- die Emulation liefert 0 W "
                     "und verstummt nach der eingestellten Überbrückung")
    if live["conn"] < live["dev"]:
        notes.append(f"{live['dev'] - live['conn']} Gerät(e) antworten nicht "
                     f"-- die angezeigten Summen sind unvollständig")
    if live["polls"] and live["err"] / max(live["polls"], 1) > ERR_RATIO_WARN:
        notes.append(f"{live['err']} Fehler auf {live['polls']} Abfragen")
    if meter["manip"]["en"]:
        notes.append("Phasenmanipulation ist AKTIV -- der Deye bekommt veränderte Werte")

    print()
    if notes:
        for n in notes:
            print(f"  ! {n}")
    else:
        print("  alles unauffällig")

    if tail:
        print(f"\n--- letzte {tail} Logzeilen ---")
        try:
            with urllib.request.urlopen(f"http://{ip}/log", timeout=TIMEOUT) as r:
                lines = r.read().decode("utf-8", "replace").splitlines()
            for line in lines[-tail:]:
                print("  " + line)
        except (urllib.error.URLError, OSError) as e:
            print(f"  /log nicht abrufbar: {e}")

    return 1 if notes else 0


if __name__ == "__main__":
    sys.exit(main())

# Coredumps

Abgeholt mit `curl -s http://<ip>/coredump -o <datei>.bin`. `python3 scripts/health.py`
meldet von sich aus, wenn einer vorliegt. **Erst abholen, dann** mit
`GET /coredump?erase=1` Platz für den nächsten machen.

## Auswerten

`espcoredump.py` lehnt eine Firmware ab, deren SHA256 nicht exakt die des
abgestürzten Abbilds ist — und bitgleich wird ein Nachbau nie, weil
`build_number.py` einen Zeitstempel einkompiliert. Der Umweg ist trivial und
hat keinen Nachteil: die Datei ist eine 24-Byte-Kopfzeile vor einem regulären
RISC-V-ELF-Core. Abschneiden, und gdb lädt ihn ohne jede Prüfung.

```bash
# 1. Das ELF des abgestuerzten Builds nachbauen (Build-Nummer steht in /ota).
git worktree add /tmp/b274 <commit>
#    version.json so setzen, dass das Vor-Skript auf genau diese Nummer zaehlt
#    -- nur dann stimmen die Zeichenkettenlaengen und damit die Adressen.
cd /tmp/b274 && pio run

# 2. Kopfzeile abschneiden.
python3 -c "d=open('dump.bin','rb').read(); open('core.elf','wb').write(d[24:])"

# 3. Laden.
GDB=~/.platformio/packages/tool-riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb
$GDB -batch -n -ex "set pagination off" -ex "thread 1" -ex "bt 30" \
     /tmp/b274/.pio/build/guition-p4/firmware.elf core.elf
```

Tasknamen stehen nicht in der gdb-Threadliste, lassen sich aber aus den TCBs
lesen — die „process id" eines Threads **ist** die TCB-Adresse:

```
x/s ((TCB_t*)<pid>)->pcTaskName
```

**Was nicht drin ist:** der Coredump enthält die Task-Stacks, aber **keine
Heaps** und kein `.data`. `registered_heaps` liest sich deshalb als 0 (das
kommt aus dem ELF, nicht aus dem Abbild), und PSRAM-Adressen sind gar nicht
zugreifbar. Bei einer Heap-Beschädigung sieht man also den Stolperer, nie den
Verursacher.

## Geschwärzt vor dem Veröffentlichen

Dieses Repository ist öffentlich, ein Coredump ist ein Speicherabbild. Vor dem
Einchecken geprüft und **nicht** enthalten: WLAN-Passwörter, MQTT-Passwort,
WireGuard-Schlüssel, AP-Passwort — Stacks allein tragen die offenbar nicht.

Zwei identifizierende Zeichenketten standen drin und sind **längengleich
überschrieben** mit `<<<geschwaerzt>>>`: die SSID des Heimnetzes und die MAC
des Geräts. Beide liegen im Stack des WLAN-Tasks und spielen für die
Heap-Frage keine Rolle. Nachgewiesen: derselbe gdb-Aufruf liefert vor und nach
der Schwärzung einen **byteidentischen** Stapel, und die Tasknamen aus den TCBs
sind weiterhin lesbar. Die privaten IP-Adressen (`192.168.177.x`) stehen
ohnehin schon in `scripts/health.py` und im Wiki und sind drin geblieben.

Wer einen neuen Dump eincheckt: **vorher durchsuchen.** Die Zeichenketten, auf
die es ankommt, stehen in der Einstellungs-Sicherung (`GET /config`).

## Vorliegende Abbilder

| Datei | Was |
| --- | --- |
| `2026-09-11-mqtt_task-tlsf.bin` | 11.09.2026 ~08:02, Build 274, nach 15,5 h Laufzeit. TLSF-Assertion beim Allozieren im `mqtt_task`. Aufgeloester Stapel in der `.txt` daneben; Befund im [Code-Review](../../wiki/Code-Review-2026-09.md), Nachtrag 13. |

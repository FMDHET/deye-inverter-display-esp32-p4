# Energiemanager

Der Energiemanager plant den Deye-Akku nach dem **dynamischen Strompreis** (Tibber), der **PV-Prognose** und dem **gelernten Hausverbrauch**. Er lädt in billigen Viertelstunden aus dem Netz, hält die Energie für teure Spitzen zurück und sperrt dabei auf Wunsch die Entladung des BYD am Fronius, damit der nicht in den Deye speist.

Bedient wird er im Tab **„Energie“** auf `http://<ip-des-geräts>/deye`, per REST-API unter `/api/em/…` und per MQTT mit Home-Assistant-Discovery.

> [!IMPORTANT]
> Ab Werk steht der Energiemanager auf **„Aus“**. „Nur planen“ zeigt, was er tun würde, ohne einzugreifen. Erst **„Automatik“** steuert den Deye — und den BYD nur, wenn „BYD-Entladung sperren erlaubt“ eingeschaltet ist.

## Betriebsarten

| Betriebsart | Daten holen | Plan | Deye | BYD |
| --- | --- | --- | --- | --- |
| Aus | ja | nein | unberührt | unberührt |
| Nur planen | ja | ja | unberührt | unberührt |
| Automatik | ja | ja | lädt / hält nach Plan | Entladesperre, wenn erlaubt |

Preise, Prognose und Lastprofil laufen in jeder Betriebsart: Die Seite zeigt sie immer, und das Lastprofil lernt weiter.

## Datenquellen

| Was | Quelle | Wie oft |
| --- | --- | --- |
| Strompreis (Endpreis inkl. Netzentgelt, Abgaben, Steuer) | Tibber, `priceInfo(resolution: QUARTER_HOURLY)` | nach dem Start, um 00:05, ab 13:00 halbstündlich bis die Preise für morgen da sind, sonst alle 6 h |
| Ersatz, wenn Tibber nicht antwortet | Energy-Charts (Börsenpreis DE-LU); Endpreis = Börse × 1,19 + Aufschlag, den die Firmware aus dem letzten Tibber-Abruf lernt (sonst 20 ct) | höchstens alle 2 h |
| Strommix (Anteil Erneuerbare, Sonne, Wind an Land/auf See) | Energy-Charts `ren_share_forecast` | alle 3 h |
| Wetter für die PV-Prognose | Open-Meteo, je Viertelstunde: Globalstrahlung, Diffusstrahlung, Direktnormalstrahlung, Temperatur, Wind; stündlich Schneehöhe. Eine Abfrage für alle Felder | stündlich |
| Hausverbrauch | eigener Messwert, je Viertelstunde in den Langzeitspeicher; Profil = Durchschnitt der letzten ~6 Wochen | laufend |

**Datenquellen** (aufklappbar unter dem Diagramm „Preise & Plan“): je Quelle der Zustand („✓ erreichbar“, „✕ nicht erreichbar“ mit Fehlertext, „⏸ gebremst“ bei HTTP 429, „… wird geholt“), die letzte gute Antwort, der nächste Abruf und was die Antwort abdeckt (nur heute / heute + morgen). Die Kopfzeile sagt auch zugeklappt, ob alle erreichbar sind. „Nur heute“ bei Energy-Charts ist am Nachmittag normal: Morgen veröffentlicht Energy-Charts meist erst am Abend, für heute nur 00–24 Uhr UTC (also ab 01:00 bzw. 02:00 Ortszeit).

Die Erzeugungsprognose in GW (Wind an Land, Wind auf See, Sonne) holt die Firmware je Art zweimal: „day-ahead“ und danach „intraday“. Day-ahead liefert für heute nur, bis am Nachmittag die Prognose für morgen erscheint, danach nur noch morgen. Intraday deckt heute ab und überschreibt day-ahead dort mit den neueren Zahlen. Ein leerer Wert überschreibt nie einen bekannten. Eine Runde sind sechs Abfragen im Minutenabstand, gut 5 Minuten. Die Sonne kommt zuerst, nach einem Neustart steht sie nach knapp 2 Minuten. Nach einem Neustart erscheint außerdem jede Art schon mit ihrem day-ahead-Teil, damit der Plan nicht minutenlang ohne Sonne oder Wind dasteht. Danach wird erst mit beiden Teilen umgeschaltet, sonst verschwände abends kurz der heutige Tag.

Energy-Charts begrenzt die Abfragen streng (HTTP 429). Die Firmware fragt dort deshalb selten und wartet nach einem 429 mindestens eine Stunde. Echte Tibber-Preise werden nie von der Energy-Charts-Schätzung überschrieben.

Jeder HTTPS-Abruf braucht für den TLS-Handshake rund 45 kB freien internen Speicher. Der freie Speicher schwankt im Betrieb um 10–15 kB (gemessen 38–52 kB). Deshalb wartet ein Abruf bis zu 30 s auf einen günstigen Moment. Klappt es in der Zeit nicht, zeigt die Seite „verschoben, Speicher gerade knapp“ statt „keine Verbindung“, und der Abruf wird später wiederholt.

**Der Tibber-Token** wird im Tab „Energie“ eingetragen. Er liegt im NVS, ist in der Einstellungssicherung enthalten (wie die WLAN-Passwörter) und wird über keine Schnittstelle wieder ausgegeben — die Seite zeigt nur „gesetzt“.

## PV-Prognose und Kalibrierung

Für jedes PV-Feld (bis zu sechs) werden **kWp, Neigung und Ausrichtung** (Kompassgrad: 0 Nord, 90 Ost, 180 Süd, 270 West) eingetragen, optional die **AC-Grenze** des Wechselrichters (0 = keine), dazu der **Standort** (Ortssuche im Tab) und die **Messung**, die dieses Feld erfasst: ein Modbus-TCP-Gerät (z. B. der Wechselrichter „Ost“) oder die eigenen PV-Strings des Deye.

Das Modell ist das des Betreibers (seine Home-Assistant-Sensoren, übernommen am 2026-10-22). Die Firmware rechnet es je Feld und Viertelstunde selbst, aus dem Wetter am Standort:

| Schritt | Verfahren |
| --- | --- |
| Sonnenstand | Spencer (Deklination, Zeitgleichung), zur Mitte der Viertelstunde |
| Einstrahlung auf die Modulebene | HDKR (Hay-Davies-Klucher-Reindl) aus GHI, DHI, DNI; Bodenreflexion mit Albedo 0,2, bei mehr als 1 cm Schnee 0,65 |
| Reflexionsverlust | Martin-Ruiz-IAM (a_r 0,16), nur auf den Direktanteil |
| Zelltemperatur | Faiman: T_Zelle = T_Luft + POA / (25 + 6,84 · Wind) |
| Modulwirkungsgrad | Huld (PVGIS-Koeffizienten c-Si) |
| Verluste | 8 % pauschal (Faktor 0,92) |
| Schnee | unter 60° Neigung: > 5 cm × 0,05, > 2 cm × 0,3, > 0,5 cm × 0,7; die Fassade nicht |

```text
P = min(kWp × POA/1000 × η_Huld × 0,92 × Schneefaktor × Korrekturfaktor, AC-Grenze)
```

Weil das Wetter für alle Ausrichtungen dasselbe ist, genügt **eine** Abfrage für alle Felder, und eine geänderte Ausrichtung wird sofort aus dem gespeicherten Wetter neu gerechnet. Die Host-Tests prüfen die Firmware gegen eine Python-Fassung des Modells (auf 0,5 % genau); „Prognose testen“ im Browser rechnet dasselbe Modell. Anders als in Home Assistant kommen Temperatur und Wind nur von Open-Meteo, nicht von der eigenen Wetterstation.

Der **Korrekturfaktor** lernt sich selbst: Jeden Tag vergleicht die Firmware die gemessene mit der vorhergesagten Energie je Messung (Felder an derselben Messung zählen gemeinsam) und zieht den Faktor um 20 % der Abweichung nach, begrenzt auf 0,3…1,5. Gezählt wird ein Tag nur, wenn die Messung mindestens 70 % der vorhergesagten Energie abgedeckt hat. Darin stecken dann Verschattung und Verschmutzung. Die AC-Grenze gilt schon vor dem Vergleich: In Stunden, in denen der Wechselrichter abregelt, liefert er genau die Grenze, die Prognose sagt genau die Grenze. Der Faktor lernt dann nicht, das Feld bringe zu wenig. Ein Faktor über 1 hebt die Prognose nie über die Grenze. Ändert sich die Geometrie eines Felds, beginnt sein Faktor wieder bei 1,0.

„Prognose testen“ in den Einstellungen rechnet die Vorschau direkt im Browser, noch ohne Kalibrierung.

### Gemessen gegen Prognose

Die Firmware hält für den laufenden Tag je PV-Feld und Viertelstunde fest, wie viel Energie gemessen wurde und wie viel der Viertelstunde die Messung abgedeckt hat. Grundlage ist dieselbe Messung wie für die Kalibrierung.

* **PV-Prognose:** Die schwarze Linie „gemessen (Summe)“ liegt über der gestapelten Prognose. Der Tooltip zeigt je Feld Prognose und Messung.
* **PV-Tagesenergie:** Ist kumuliert (Fläche) gegen Prognose kumuliert (Linie), je Tag ab Mitternacht. Verglichen werden nur Felder mit eigener Messung. Darüber stehen Ist und Prognose bis jetzt, der Anteil in % und die Prognose für den ganzen Tag. Die Prognose ist die aktuelle, stündlich neu geholte und kalibrierte, nicht die vom Morgen.
* **Nachts:** Die Fronius-Wechselrichter schalten nach Sonnenuntergang ab und antworten nicht mehr. Wenn die Prognose Nacht sagt, zählt ein schweigender Wechselrichter als 0 W gemessen. Tagsüber bleibt er eine Lücke.
* **Lücken:** Fehlt Messung (Display aus, Wechselrichter tagsüber nicht erreichbar), ist Ist zu niedrig. Die Seite sagt das dann: „Messung lückenhaft (x % des Tages erfasst)“.
* **Neustart:** Der Tagesverlauf liegt in einem PSRAM-Bereich, der beim Start nicht gelöscht wird (`CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY`). Ein Software-Neustart oder ein OTA übersteht er, gültig nur mit Kennung, Prüfsumme und gleichem Datum. Ein Stromausfall löscht ihn. Wechselt ein Feld die Messung, beginnt sein Verlauf neu; eine geänderte Ausrichtung behält ihn.

## Der Plan

Alle 15 Minuten (und sobald neue Preise oder eine neue Prognose da sind) plant die Firmware vom aktuellen Viertel bis zum Ende der bekannten Preise — bis zu 48 Stunden. Je Viertelstunde gibt es drei Aktionen:

| Aktion | Deye | wie |
| --- | --- | --- |
| Normal | Überschuss lädt, Bedarf entlädt | — |
| Halten | entlädt nicht, nimmt aber Überschuss auf | über die Zähler-Emulation, ohne Registerschreiben |
| Laden | lädt zusätzlich mit der eingestellten Leistung aus dem Netz | Zwangsladung mit Ziel-SoC |

**Halten wird immer vor Laden versucht:** Es kostet keine Umwandlung. Laden lohnt sich nur, wenn

```text
billig ÷ (η_Laden × η_Entladen) + Verschleiß + Mindestvorteil < teuer
```

Die Wirkungsgrade stammen aus der Messung dieses Wechselrichters (fester Verlust ~0,1 kW plus ein quadratischer Anteil). Die PV-Prognose geht vorsichtig mit 80 % ein, damit nicht aus dem Netz geladen wird, was die Sonne ohnehin bringt. **Nach dem Ende der Preise** rechnet der Plan mit PV-Prognose und Verbrauchsprofil weiter, bis die Wetterprognose endet (morgen 24 Uhr), und danach bis 48 h ab jetzt mit „morgen wie heute“ (PV derselben Viertelstunde einen Tag vorher). Entscheidungen fallen dort keine, aber daraus ergibt sich, was Energie am Ende der Preise wert ist: nur so viel, wie sie danach an Netzbezug spart, zum Median-Preis. Was selbst danach noch im Akku steckt, zählt nur zum **unteren Quartil** der bekannten Preise. So weit voraus ersetzt eine kWh eher eine billige als eine durchschnittliche. Zum Median hat das an einem Tag mit flachen Preisen nächtliches Halten eines vollen Akkus lohnend aussehen lassen.

* **Sonniger nächster Tag:** Die Sonne füllt den Akku am Morgen ohnehin. Übrige Energie ist dann nur die Nacht bis Sonnenaufgang wert, also wird nur dafür geladen.
* **Trüber nächster Tag:** Der ganze Tag zählt, und günstig zu laden lohnt sich.
* **Viel Sonne heute:** Was heute hereinkommt und morgen reicht, wird nicht im Voraus gekauft, und der Abend hortet nichts unnötig.
* **Ohne Prognose** dahinter gilt wie früher ein fester Wert je kWh (Median-Preis).

**Sättigung:** Ab 90 % SoC nimmt der Akku im Modell linear weniger auf, bei 100 % nur noch 20 % der Ladeleistung (Konstantspannungsphase). Das gilt für PV und Netz gleichermaßen und wird innerhalb der Viertelstunde exakt aufintegriert. Der Plan rechnet also nicht damit, die letzten Prozent mit voller Leistung zu laden, und fängt früh genug an.

Der Planer ist ein „Simulieren-und-Übernehmen“-Verfahren: Er nimmt die teuerste Viertelstunde mit Netzbezug, probiert für jede billigere davor Halten, 0,25 kWh mehr Laden oder die ganze Viertelstunde mit voller Ladeleistung, simuliert neu und übernimmt den besten Zug, wenn er mehr als den Mindestvorteil spart. Der Zug „volle Viertelstunde“ ist nötig, weil der Eigenverbrauch des Wechselrichters beim Laden (~0,1 kW) einen ersten 0,25-kW-Schritt halb auffrisst. Ohne ihn blieb eine billige Viertelstunde ungenutzt, und später wurde teurer zugekauft. Was zu viel ist, nimmt das Zurücknehmen Schritt für Schritt wieder weg. Gegen einen exakten Optimierer erreicht er in Tests 94–99 % der möglichen Ersparnis, in 50–160 Durchgängen.

## Verlauf: was wirklich war

Der Plan beginnt immer bei der laufenden Viertelstunde. Damit im Diagramm „Preise & Plan“ die Vergangenheit nicht verschwindet, schreibt die Firmware am Ende jeder Viertelstunde einen Eintrag in einen eigenen 1-MB-Bereich im Flash (0xB00000, `emhist`, data 0x41, 32 Byte je Viertelstunde, Platz für rund 340 Tage; danach fallen die ältesten weg). Darin stehen:

* Deye-SoC und BYD-SoC am Ende der Viertelstunde
* Mittelwerte von Deye-Akku (+ = Entladen), Netz (+ = Bezug), PV und Hausverbrauch
* der Endpreis
* was für die Viertelstunde geplant war (Normal, Halten, Laden), ob die Automatik es ausgeführt hat und die Betriebsart

Das Diagramm zeigt links von „jetzt“ den **gemessenen** SoC (Linie in Schriftfarbe) und den Lade-/Haltestreifen aus dem Verlauf, rechts davon den **erwarteten** SoC aus dem Plan. Ein Streifen, der nur geplant und nicht ausgeführt wurde (Schattenbetrieb, Handbedienung), ist blass. Im Tooltip steht dann „nur geplant“.

Der Eintrag landet erst am Ende der Viertelstunde im Flash. Bei einem Neustart oder Stromausfall geht nur die gerade laufende Viertelstunde verloren. Geräte ohne den Bereich in der Partitionstabelle binden ihn wie den Langzeitspeicher zur Laufzeit ein.

## Verbrauchsprofil und Langzeitspeicher

Der Hausverbrauch wird je Viertelstunde gemessen (Mittel über die Viertelstunde, nur wenn mindestens 70 % davon Messwerte hatten). Um Mitternacht kommt der Tag in den **Langzeitspeicher**: ein eigener 1-MB-Bereich im Flash (0xA00000, 256 Byte je Tag, Platz für rund 11 Jahre).

* **Profil:** je Viertelstunde der Durchschnitt der letzten 30 Werktage bzw. 12 Wochenendtage (je etwa 6 Wochen), als getrimmtes Mittel. Die höchsten und niedrigsten 10 % fallen weg, sodass Feiertage, Feste oder Urlaub das Profil nicht verbiegen. Bis ein Tagestyp 3 gespeicherte Tage hat, gilt das frühere gleitende Mittel.
* **Neustart und Update:** Der laufende Tag liegt in einem PSRAM-Bereich, der einen Software-Neustart und jedes OTA übersteht. Ein Stromausfall kostet nur den Teil des Tages bis dahin. War das Display über Mitternacht aus, wird der Vortag beim nächsten Start nachgetragen.
* **Ohne Partitionstabelle:** Geräte, die nur per OTA aktualisiert werden, haben den Bereich nicht in ihrer Partitionstabelle. Die Firmware bindet ihn dann zur Laufzeit ein (`esp_partition_register_external`). In `partitions.csv` steht er an derselben Stelle (`loadhist`, data 0x40), für das nächste Flashen per USB.
* **Ansicht:** Die Karte „Verbrauchsprofil“ zeigt die Quelle (Durchschnitt aus n Tagen oder gleitendes Mittel), die Zahl der gespeicherten Tage seit dem ersten und die heute gemessene Kurve.

## Was im Automatikbetrieb passiert

* **Laden:** Zwangsladung mit der geplanten Leistung. Der **Ziel-SoC wird in die Zeitfenster-Register des Deye geschrieben** — der Wechselrichter hört dort von selbst auf, auch wenn das Display ausfällt. Das Fenster ist so lang wie der Ladeblock plus 10 min (höchstens 12 h); danach schaltet die Akku-Steuerung von sich aus auf Normal.
* **Halten:** Die Zähler-Emulation meldet dem Deye `min(Netz − Sollwert, −seine Abgabe)`. Gibt er ab, regelt er dadurch auf 0 W herunter; lädt er aus einem Überschuss, gilt die normale Regelung. Das Halten wird alle 10 s erneuert und **läuft nach 2 min ohne Erneuerung von selbst ab**. Bei veraltetem Zählerwert gilt weiterhin die alte Regel: 0 W.
* **BYD (nur wenn erlaubt):** Während Laden oder Halten wird die Entladung am Fronius gesperrt (SunSpec Model 124: Entladegrenze OutWRte 0 %). Jeder Schreibvorgang wird zurückgelesen. Beim Aufheben stellt die Firmware die vorher gelesenen Werte wieder her (am Gerät: StorCtl_Mod 3, OutWRte 26,04 % ≈ 2 kW).
  Der Symo Hybrid hier kennt **keine Rückfallzeit** (RvrtTms liest 0xFFFF). Die Sperre hebt sich also nicht von selbst auf. Deshalb liegt der Zustand samt Originalwerten im NVS (`em/fsblk`), und nach einem Neustart des Displays wird die Sperre mit den echten Originalwerten aufgehoben (am Gerät geprüft). Fällt das Display ganz aus, bleibt die Sperre bestehen, bis sie im Fronius aufgehoben wird.
* **Du hast Vorrang:** Wird am Display, per MQTT oder auf der Werkbank eine Betriebsart oder Leistung gesetzt, pausiert die Automatik bis zum Ende des laufenden Blocks und überschreibt nichts. Während eines [Leistungsdurchlaufs](Deye-Steuerung#leistungsdurchlauf) ruht sie ebenfalls.

## Akkus sperren (Deye und BYD)

In der Statuskarte des Tabs „Energie“ stehen beide Akkus untereinander unter **„Akkus sperren“**: Zustand, Dauer (bis aufgehoben oder 1, 2, 4, 8, 12 Stunden), „Sperren“ und „Freigeben“. Gesperrt gibt ein Akku nichts ab, lädt aber weiter aus PV-Überschuss, etwa damit beim Laden des Autos kein Hausakku ins Auto wandert.

| | Deye | BYD |
| --- | --- | --- |
| Wie | Zähler-Emulation („Halten“), keine Register | Entladesperre am Fronius (OutWRte 0 %), zurückgelesen |
| Fällt das Display aus | nach 2 Minuten wieder frei | bleibt gesperrt (der Fronius hat keine Rückfallzeit) bis das Display wieder läuft oder im Fronius aufgehoben wird |
| Voraussetzung | – | „BYD-Steuerung erlaubt“ |

Gemeinsam für beide:

* **Unabhängig vom Energiemanager:** Jede Sperre ist eine eigene Anforderung neben dessen „Halten“ bzw. BYD-Sperre. Der Energiemanager hebt sie nicht auf, und umgekehrt.
* **Zeit:** Die Sperrzeit hält der ESP selbst. Beide Sperren stehen im NVS und überstehen Neustart und Update. Eine befristete Sperre endet nach Ablauf von selbst, beim BYD mit den vorherigen Werten (zurückgelesen).
* **Home Assistant:** Schalter „Deye-Akku sperren“ und „BYD-Akku sperren“ (`<basis>/em/deye_lock/set`, `…/byd_lock/set`, `1`/`0`, gilt bis aufgehoben) und je ein Sensor „… gesperrt bis“. *Retained* Befehle werden ignoriert. Für die Wallbox: eine Automation schaltet beide mit dem Ladezustand des Autos.

## Eine Verbindung je Fronius

Livewerte (PV, BYD-Leistung, SoC) und die Akkusteuerung (Sperre, Ladegrenze, Zwangsbetrieb, Regler) laufen über **dieselbe** Modbus-TCP-Verbindung. Die Zählerabfrage hält sie dauerhaft offen. Für einen Lese- oder Schreibvorgang leiht sich die Akkusteuerung diese Verbindung kurz aus, die Abfrage dieser Adresse wartet so lange. Bricht dabei eine Antwort ab, wird die Verbindung neu aufgebaut, damit keine verspätete Antwort in die nächste Abfrage gerät.

Früher öffnete die Akkusteuerung für jeden Vorgang eine zweite, kurze Verbindung. Wenn die 16 Sockets des Displays gerade knapp waren, schlug genau dieser Verbindungsaufbau fehl, und die Karte meldete „Keine Verbindung“, obwohl die Livewerte weiterliefen.

## Fronius-Werte von Hand, obere Ladegrenze

In der Karte „Fronius / BYD“ stehen neben **Reserve (MinRsvPct)**, **Entladen (OutWRte)** und **Laden (InWRte)** Schieberegler, 0–100 %. Bei den Raten steht die Leistung in W daneben. Geschrieben wird beim Loslassen und danach zurückgelesen. Das geht nur mit „BYD-Steuerung erlaubt“.

* **Grundwert während einer Sperre:** Hält der ESP gerade selbst ein Register (Entladesperre, Ladegrenze, Zwangsbetrieb), stellt der Regler den **Grundwert** ein, also den Wert, der nach dem Ende zurückkommt. Das Register selbst bleibt so lange, wie der ESP es braucht. Die Zeile zeigt beides, z. B. „26,04 % (2.000 W) · jetzt 0,00 % (Sperre)“. Der Grundwert liegt im NVS und übersteht einen Neustart.
* **Negative Raten** (also Zwangsbetrieb) gibt es nur über „Zwangsbetrieb“, nicht über die Regler.

**Obere Ladegrenze:** Modell 124 kennt keinen maximalen Ladestand, nur die Reserve nach unten. Die Grenze hält deshalb der ESP.

* Erreicht der BYD den eingestellten Wert, setzt er InWRte auf 0 % (Laden aus); 2 Prozentpunkte darunter gibt er wieder frei.
* Einstellbar 50–100 %, 100 % = aus. Der Wert wird in den Einstellungen gespeichert (`byd_max`).
* Mit der Entladesperre zusammen sind beide Richtungen gesperrt.
* Die Grenze gilt **immer**, auch für Zwangsladen. Liegt dessen Ziel höher, endet es an der Grenze („Ladegrenze erreicht“).
* Einstellbar auch in der Statuskarte unter „Ladegrenze“ (aus, 95 … 50 %).
* Wie bei der Sperre gilt: Fällt das Display dauerhaft aus, bleibt das Laden gesperrt, bis es im Fronius freigegeben wird.

## Zwangsbetrieb (Deye und BYD)

In der Statuskarte des Tabs „Energie“ steht unter **„Zwangsbetrieb“** je Akku eine Zeile: Richtung (Laden/Entladen), Leistung, Dauer (1–720 min), Ziel-Ladestand, „Starten“ und „Beenden“, darunter der Zustand. Geladen wird auch aus dem Netz. Der Zwangsbetrieb endet nach der Dauer oder beim Ziel-Ladestand, was zuerst kommt, und mit jedem Neustart des Displays. Der Energiemanager hält sich so lange heraus.

**Deye:**

* Läuft über dieselbe Steuerung wie das Laden des Energiemanagers (Register 142/143 bzw. 126–128): 250–20.000 W.
* Die Dauer hält der Deye selbst, er fällt danach auf Normal zurück. Beim Laden steht der Ziel-Ladestand in den Zeitfenster-Registern des Deye, er hört dort also auch ohne Display auf.
* Beim Entladen gibt es dafür kein Register; den Ladestand überwacht das Display.
* Wird die Betriebsart am Display oder per Home Assistant geändert, endet der Zwangsbetrieb ohne Rückschreiben.

**BYD:** Laden auch aus dem Netz (ChaGriSet = 1). Das braucht „BYD-Steuerung erlaubt“.

* **Register:** beide Grenzen in % von WChaMax, mit gleichem Betrag und entgegengesetztem Vorzeichen. Laden = InWRte +P, OutWRte −P. Entladen = OutWRte +P, InWRte −P. StorCtl_Mod 3. Die Akkuleistung muss zwischen −InWRte und +OutWRte liegen; mit einer negativen Grenze bleibt genau P übrig. Quelle sind Community-Beschreibungen ([libe.net](https://libe.net/en/byd-modbus), [loxforum](https://www.loxforum.com/forum/faqs-tutorials-howto-s-aa/132379-fronius-hybrid-with-modbus-tcp)), kein Fronius-Dokument. Jeder Schreibvorgang wird zurückgelesen.
* **Reihenfolge:** Erst die Grenze, die sich öffnet; sie wird zurückgelesen, bevor die schließende geschrieben wird. Dann die Freigabebits. So entsteht nie ein leerer Leistungsbereich, auch wenn der Fronius einen Wert ignoriert.
* **Ende:** nach der Dauer (1–720 min, Vorgabe 60) oder beim Ziel-Ladestand (Laden ≥, Entladen ≤), was zuerst kommt. Der ESP wacht selbst darüber, weil der Fronius keine Rückfallzeit hat. Ein Neustart des Displays beendet den Zwangsbetrieb ebenfalls: Die Originalwerte, InWRte eingeschlossen, kommen aus dem NVS zurück. Wird „BYD-Steuerung erlaubt“ ausgeschaltet, endet er sofort.
* **Vorrang:** Der Zwangsbetrieb geht vor die Sperre des Energiemanagers. Endet er, gilt wieder die Sperre (falls der Plan sie will) oder die Einstellung des Fronius.
* **Rollback:** Solange InWRte nicht zurückgeschrieben ist, lehnt `/ota/rollback` ab. Eine ältere Firmware würde die Sperrbits zurücksetzen, InWRte aber stehen lassen.

> [!WARNING]
> Der Zwangsbetrieb ist am Gerät noch nicht erprobt. Den ersten Lauf bitte mit kleiner Leistung und kurzer Dauer machen und dabei Fronius-App bzw. Solar.web beobachten. Zu prüfen ist, ob der Fronius die negativen Grenzen annimmt und wirklich mit P lädt oder entlädt.

## Einstellungen

| Feld | Vorgabe | Bereich |
| --- | --- | --- |
| Standort | — | Breite ±90°, Länge ±180° |
| PV-Felder | — | bis 6; 0,1…100 kWp, Neigung 0…90°, Ausrichtung 0…359°, AC-Grenze 0 (keine)…60 kW |
| Deye / BYD Kapazität | — | 0…500 kWh (ohne Deye-Kapazität kein Plan) |
| BYD-Reserve | 0 % | 0…100 % |
| Ladeleistung | 3 kW | 0,25…20 kW — ab ~2,5 kW lädt der Deye mit ~95 % |
| Mindestvorteil | 3 ct/kWh | 0…100 |
| Verschleiß | 2 ct/kWh | 0…100 |
| SoC min / max | 15 / 95 % | max mindestens 10 Punkte über min |

Gespeichert im NVS (Namensraum `em`, Schlüssel `cfg`) und in der Einstellungssicherung. Lastprofil (`em/load`) und PV-Korrekturfaktoren (`em/pvcal`) sind gelernter Zustand und nicht Teil der Sicherung; das Lastprofil wird nicht geschrieben, solange das NVS fast voll ist.

## REST-API

| Methode | Adresse | Inhalt |
| --- | --- | --- |
| `GET` | `/api/em/cfg` | Einstellungen, PV-Felder als `planes[]{name,kwp,tilt,az,meter,ac}` (`ac` in kW, 0 = keine Grenze); Token nur als `"tok":0/1` |
| `POST` | `/api/em/cfg` | JSON mit denselben Feldern; `"token"` fehlt = behalten, `""` = löschen |
| `GET` | `/api/em/prices` | `src`, `fetched`, `err`, `tomorrow`, `t0`, `step`, `total[]`, `energy[]` (ct/kWh), `mix{t0,step,ren[],solar[],wind_on[],wind_off[],wind_gw[],solar_gw[]}`, `sources[]` je Quelle: `name`, `host`, `used`, `last_try`, `last_ok`, `next`, `fails`, `r429`, `tomorrow`, `stage`, `err` |
| `GET` | `/api/em/forecast` | `total[]` kW, `meas_t0`, `planes[]{name,cal,kwh_today,kwh_tomorrow,meas_kwh_today,kw[],mkwh[],mcov[]}` — `mkwh` gemessene kWh je Viertelstunde ab `meas_t0` (null = keine Messung), `mcov` Abdeckung in % |
| `GET` | `/api/em/load` | `wd[96]`, `we[96]` kW (das Profil, mit dem geplant wird), `src[2]` (1 = Durchschnitt aus dem Langzeitspeicher), `avg_days[2]`, `hist_ok`, `hist_days`, `hist_from`, `today[96]` (heute gemessen), `days_wd`, `days_we` (gleitendes Mittel) |
| `GET` | `/api/em/hist?from=<unix>&n=<slots>` | Verlauf je Viertelstunde ab `from` (ohne: ab Mitternacht, bis jetzt; höchstens 192): `t0`, `step`, `found`, `total`, `oldest`, `act[]` (0 Normal, 1 Halten, 2 Laden), `flags[]` (Bit 0 ausgeführt, Bit 1 pausiert, ab Bit 4 Betriebsart), `soc[]`, `byd[]` in %, `bat[]`, `grid[]`, `pv[]`, `house[]` in W, `price[]` in ct; `null` = nicht aufgezeichnet |
| `GET` | `/api/em/loadhist?off=0&n=14` | gespeicherte Tage, neueste zuerst (höchstens 31 je Abruf): `total`, `days[]{date,wd,kwh,w[96]}` mit `w` in W je Viertelstunde, `null` = nicht gemessen |
| `GET` | `/api/em/plan` | `mode`, `act[]` (0 Normal, 1 Halten, 2 Laden), `kw[]`, `soc[]`, `grid[]`, `price[]`, `for[]`, `cost_plan`, `cost_base` (ct), `now{act,why,price,ren,solar,wind}`, `saving`, `pv_today`, `pv_tomorrow`, `deye_soc`, `next_charge[from,to]`, `byd{ok,soc,blocked,no_revert,forcing,force,cap,cap_on,err}` |
| `GET` | `/api/em/fronius` | Model-124-Werte des Fronius, `blocked`, `no_revert`, `owed`, Zwangsbetrieb: `forcing` (übernommen), `force` (0 aus, 1 laden, 2 entladen), `force_w`, `force_until`, `force_soc`, `force_end`, obere Ladegrenze `cap` (100 = aus), `cap_on`, Grundwerte `base_known`, `base_out`, `base_in` (%) |
| `POST` | `/api/em/mode?m=0\|1\|2` | Aus / Nur planen / Automatik |
| `POST` | `/api/em/deye?cmd=charge\|discharge&w=…&min=…&soc=…` · `cmd=stop` | Zwangsbetrieb Deye: `w` 250–20000 W, `min` 1–720 (Vorgabe 60), `soc` 10–100 (Ende bei ≥ beim Laden, ≤ beim Entladen); Zustand in `/api/em/plan` unter `deye_force{mode,w,until,soc,active,end}` |
| `POST` | `/api/em/lock?bat=deye\|byd&on=1&min=…` · `on=0` | Akku sperren (`min` 0 oder fehlend = bis aufgehoben, sonst 1–1440) bzw. freigeben; Zustand in `/api/em/plan` unter `lock{deye{on,until,held},byd{on,until,held,allowed}}` |
| `POST` | `/api/em/refresh?what=prices\|forecast\|plan\|all` | neu holen bzw. neu planen |
| `POST` | `/api/em/fronius?cmd=block&on=0\|1&min=…` · `cmd=reserve&pct=…` | BYD-Sperre wie `/api/em/lock?bat=byd`, Reserve setzen (nur mit „BYD-Steuerung erlaubt“) |
| `POST` | `/api/em/fronius?cmd=set&reg=reserve\|out\|in&v=…` | ein Wert von Hand, `v` in % (0–100); Antwort `msg: "gilt nach Ende der Sperre"`, wenn es der Grundwert war |
| `POST` | `/api/em/fronius?cmd=charge\|discharge&w=…&min=…&soc=…` · `cmd=stop` | BYD-Zwangsbetrieb: `w` 100–10000 W (höchstens WChaMax), `min` 1–720 (Vorgabe 60), `soc` Ende bei ≥ (laden, Vorgabe 100) bzw. ≤ (entladen, Vorgabe 20) |

Alle Reihen sind Viertelstunden ab `t0` (Unix-Zeit), `null` heißt unbekannt.

## MQTT und Home Assistant

Zusätzlich zu den bisherigen Werten veröffentlicht das Gerät unter `<basis>/energy` (alle 30 s) und meldet per Discovery:

* **Sensoren:** Strompreis (mit dem Preisverlauf als Attribut aus `<basis>/energy/prices`, für Diagrammkarten), Anteil Erneuerbare / Sonne / Wind, PV-Prognose heute / morgen, Plan jetzt (Normal/Halten/Laden), Plan-Begründung, Ersparnis laut Plan (€), nächstes Laden ab / bis, BYD-SoC, BYD-Entladung gesperrt, BYD-Zwangsbetrieb bis / zuletzt beendet.
* **Steuerung** (entfällt, wenn „Steuerung per MQTT“ aus ist): Energiemanager-Betriebsart, Netzladen-Leistung, Netzladen bis SoC, Mindestvorteil, BYD-Steuerung erlaubt, Knöpfe „Preise neu holen“ und „Neu planen“. Dazu Deye- und BYD-Zwangsbetrieb je als Auswahl (Aus/Laden/Entladen) mit den Zahlen Leistung, Dauer und „bis SoC“. Die Zahlen sind nur die Parameter, gestartet wird über die Auswahl. Ein *retained* Startbefehl wird ignoriert, sonst würde ihn der Broker bei jeder Neuverbindung erneut auslösen. Befehle gehen an `<basis>/em/<feld>/set`.

## Grenzen

* Der BYD wird im Plan nicht modelliert — nur seine Entladung gesperrt, wenn der Deye lädt oder hält. Was er sonst tut, steckt im gemessenen Hausverbrauch.
* Einspeisevergütung ist mit 0 ct angesetzt.
* Flexible Verbraucher (Wallbox, Wärmepumpe, Heizstab) und Netzvorgaben (§ 14a, negative Preise abregeln) sind noch nicht eingebunden.

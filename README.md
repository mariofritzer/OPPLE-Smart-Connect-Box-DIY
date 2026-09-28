# 💡 SkyBridge – Bluetooth-Mesh-Lampen in Home Assistant

SkyBridge macht aus einem **ESP32 für ein paar Euro** eine Brücke zwischen **Bluetooth-Mesh-Leuchten**
(z. B. **OPPLE BLE2** wie die LEDSkylight SDL-S) und **Home Assistant**. Die Lampen tauchen automatisch in
Home Assistant auf (MQTT-Discovery), mit Ein/Aus, Helligkeit und Farbtemperatur. Das alles läuft lokal, ohne Cloud und ohne Hersteller-App.

![So funktioniert SkyBridge](docs/img/uebersicht.png)

![Einrichtung in 5 Schritten](docs/img/ablauf.png)

> Zum Vergleich: Das offizielle OPPLE-Gateway (Smart Connect Box) kostet rund 790 € und hat keine Home-Assistant-Anbindung.

## Inhalt

- [Was funktioniert](#was-funktioniert)
- [Was du brauchst](#was-du-brauchst)
- [Schritt 1 – Lampen mit der Telink-App anlernen](#schritt-1--lampen-mit-der-telink-app-anlernen)
- [Schritt 2 – Mesh-Netz exportieren](#schritt-2--mesh-netz-exportieren)
- [Schritt 3 – Firmware installieren](#schritt-3--firmware-installieren)
- [Schritt 4 – SkyBridge einrichten](#schritt-4--skybridge-einrichten)
- [Schritt 5 – Home Assistant](#schritt-5--home-assistant)
- [Fehlersuche](#fehlersuche)
- [Sicherheit](#sicherheit)
- [Für Entwickler](#für-entwickler)

---

## Was funktioniert

| Funktion | |
|---|---|
| Ein/Aus, Helligkeit, Farbtemperatur (1800–12000 K) | ✅ |
| Übergangszeiten (`transition`) | ✅ |
| Jede Lampe einzeln + Gruppe „Alle Lampen“ | ✅ |
| Zustand wird regelmäßig abgefragt (auch bei Schalten per App) | ✅ alle 30 s |
| Bis zu 16 Lampen | ✅ |
| Hersteller-Szenen / Effekte (z. B. OPPLE-Himmelseffekte) | ❌ (herstellerspezifisch) |

**Geeignete Lampen:** Leuchten mit **Bluetooth SIG Mesh** und Telink-Chip, die die Standard-Modelle
*Generic OnOff* (1000), *Light Lightness* (1300) und *Light CTL* (1303) anbieten. Getestet mit
OPPLE LEDSkylight SDL-S (BLE2). Andere Telink-Mesh-Leuchten funktionieren sehr wahrscheinlich auch.

## Was du brauchst

- Ein **ESP32-Board** aus der Liste unten. Klassische Arduinos (Uno, Nano, Mega) gehen **nicht**, weil sie weder WLAN noch Bluetooth haben.
- **Home Assistant** mit **MQTT** (z. B. Add-on *Mosquitto broker*).
- Ein **Android-Handy** oder **iPhone** für die Telink-App.
- Einen PC/Mac mit **Chrome oder Edge** zum Installieren der Firmware.
- Ein USB-Netzteil für den Dauerbetrieb.

### Unterstützte Boards

| Board | Chip | Reset-Taste |
|---|---|---|
| ESP32 DevKit V1 / WROOM | ESP32 | BOOT (GPIO0) |
| LOLIN / Wemos D32 | ESP32 | BOOT (GPIO0) |
| M5Stack Atom Lite | ESP32 | Frontknopf (GPIO39) |
| ESP32-S3 DevKitC | ESP32-S3 | BOOT (GPIO0) |
| Arduino Nano ESP32 | ESP32-S3 | B1 (GPIO0) |
| Seeed XIAO ESP32-S3 | ESP32-S3 | BOOT (GPIO0) |
| LOLIN / Wemos S3 Mini | ESP32-S3 | BOOT (GPIO0) |
| M5Stack AtomS3 | ESP32-S3 | Bildschirmtaste (GPIO41) |
| ESP32-C3 SuperMini / Zero | ESP32-C3 | BOOT (GPIO9) |
| Seeed XIAO ESP32-C3 | ESP32-C3 | BOOT (GPIO9) |
| ESP32-C6 DevKit / SuperMini | ESP32-C6 | BOOT (GPIO9) |
| Seeed XIAO ESP32-C6 | ESP32-C6 | BOOT (GPIO9) |
| ESP32-C5 DevKit *(experimentell)* | ESP32-C5 | BOOT (GPIO28) |

Der **Chip wird beim Installieren automatisch erkannt**, und es wird die passende Firmware gewählt. Das Board
selbst wählst du später auf der Einrichtungsseite aus, denn davon hängen die Reset-Taste und beim XIAO ESP32-C6 die Antenne ab.

> **Tipp:** Die Beschriftung auf billigen Boards stimmt nicht immer. Ein als „C3“ verkauftes Board kann
> in Wahrheit ein C6 sein. Der Installer erkennt den echten Chip.

---

## Schritt 1 – Lampen mit der Telink-App anlernen

Die Bridge braucht die Schlüssel des Mesh-Netzes. Die Hersteller-App (z. B. OPPLE Smart) gibt diese nicht
heraus. Deshalb werden die Lampen einmalig mit der **Telink-SIG-Mesh-App** neu angelernt.

> ⚠️ Danach lassen sich die Lampen **nicht mehr mit der Hersteller-App** steuern. Wer zurück will,
> setzt die Lampen zurück und lernt sie wieder mit der Hersteller-App an.

**App installieren:**
- **Android:** [`apps/android/TelinkBleMeshDemo-V4.1.0.4.apk`](apps/android/) herunterladen und installieren
  (Installation aus dieser Quelle einmalig erlauben).
- **iPhone:** [TelinkSigMesh im App Store](https://apps.apple.com/us/app/telinksigmesh/id1536722792)

**Anlernen:**
1. Die Lampen müssen **frei** sein, also in keinem Netz:
   - bisher mit der **Hersteller-App** verwendet: dort die Lampe **entfernen/löschen**, oder die Lampe laut ihrer Anleitung auf **Werkseinstellung** zurücksetzen.
   - bisher schon mit der **Telink-App** angelernt: in der Telink-App lang auf die Lampe drücken → Reiter **SETTINGS** → **Kick out** (löschen). Die Lampe setzt sich dabei selbst zurück.
   - Tipp: Lampen, die frei sind, erscheinen in der App **nRF Connect** mit dem Dienst *Mesh Provisioning Service*.
2. Hersteller-App **komplett schließen**.
3. Die Lampen am Wandschalter **kurz aus- und wieder einschalten**. Viele Lampen lassen sich nur in den ersten Minuten nach dem Einschalten anlernen.
4. In der Telink-App auf der Startseite **Device** oben rechts auf **„+“** tippen **(1)**.
5. Unter **Device Scan** erscheinen die freien Lampen **(1)**. Lampe antippen und **ADD ALL** **(2)** drücken, dann warten, bis das Anlernen fertig ist.
6. Zurück auf der Startseite steht jede Lampe mit ihrer **Adresse**: `04(cid-27D)` bedeutet Adresse **`0004`** **(1)**.
   Antippen schaltet die Lampe, **ALL ON / ALL OFF** **(2)** schaltet alle. ✅
7. **Lang drücken** auf eine Lampe öffnet **Device Setting**: Ein/Aus **(1)**, Helligkeit **(2)** und Farbtemperatur **(3)**.

<p>
<img src="docs/img/app-1-start.png" width="200" alt="Startseite mit Plus">
<img src="docs/img/app-2-suche.png" width="200" alt="Gerätesuche">
<img src="docs/img/app-3-lampen.png" width="200" alt="Angelernte Lampe mit Adresse">
<img src="docs/img/app-4-steuerung.png" width="200" alt="Lampe steuern">
</p>

## Schritt 2 – Mesh-Netz exportieren

In der Telink-App das Netz als **JSON-Datei exportieren**:

1. Unten den Reiter **Setting** **(1)** → **Manage Network** **(2)**
2. Beim Netz *Default Mesh* auf **„•••“** **(3)** → **Share Export** **(4)**
3. Alle Net Keys angehakt lassen, **JSON File** **(5)** wählen → **EXPORT** **(6)**
4. Die Datei (z. B. `mesh.json`) auf den PC übertragen, per Mail, Cloud oder USB-Kabel.

<p>
<img src="docs/img/app-5a-einstellungen.png" width="200" alt="Setting, Manage Network">
<img src="docs/img/app-5b-netzwerk.png" width="200" alt="Network List">
<img src="docs/img/app-5c-teilen.png" width="200" alt="Share Export">
<img src="docs/img/app-6-export.png" width="200" alt="JSON exportieren">
</p>

*(Auf dem letzten Bild sind die Schlüssel absichtlich verpixelt.)*

> 🔒 Diese Datei enthält die **Schlüssel deiner Lampen**. Gib sie nicht weiter und lade sie **nie** auf GitHub hoch.

> **Gelöschte Lampen** bleiben im Export als „excluded“ stehen. Der Import auf der Einrichtungsseite überspringt sie automatisch.
> Nach Löschen und Neu-Anlernen bekommt eine Lampe eine **neue Adresse**. Dann auf der Einrichtungsseite einfach die neue Datei hochladen.

> **Fehlt eine Lampe im Export?** Manche Leuchten (z. B. OPPLE) haben alle **dieselbe Geräte-UUID**. Die
> Telink-App überschreibt dann beim Anlernen den vorherigen Eintrag. In den Lampen selbst ist trotzdem alles
> korrekt. Die Adresse jeder Lampe steht in der App auf der Startseite unter dem Lampensymbol (`04(cid-27D)` = `0004`).
> Die App vergibt die Adressen **fortlaufend** und nimmt dabei nie eine Adresse doppelt, auch nicht nach
> dem Löschen. Bei einem neuen Netz: 1. Lampe = `0002`, 2. Lampe = `0003` usw. Fehlt z. B. `0004` zwischen `0003`
> (gelöscht) und `0005`, ist das die fehlende Lampe. Eine fehlende Adresse trägst du in Schritt 4 einfach von Hand nach.

## Schritt 3 – Firmware installieren

### Variante A: Im Browser (empfohlen)

1. Board per USB anstecken.
2. Den **Web-Installer** in **Chrome oder Edge** öffnen:
   **`https://<dein-github-name>.github.io/<repo-name>/`**
   (bzw. die Datei [`docs/index.html`](docs/index.html) über GitHub Pages).
3. **„Firmware installieren“** klicken, Anschluss wählen, beim ersten Mal **„Gerät löschen“** bestätigen.

<img src="docs/img/installer-seite.png" alt="Web-Installer" width="560">

<!-- BILD: docs/img/flash-1-port.png -->
<!-- BILD: docs/img/flash-2-erase.png -->
<!-- BILD: docs/img/flash-3-fertig.png -->

### Variante B: Mit Python-Skript

```bash
python -m pip install esptool
python tools/flash.py              # sucht den Anschluss selbst
python tools/flash.py --port COM4  # oder Anschluss angeben
```

Das Skript erkennt den Chip und flasht die passende Datei aus `docs/firmware/`.

### Variante C: Von Hand mit esptool

| Chip | Datei |
|---|---|
| ESP32 | `docs/firmware/skybridge-esp32.bin` |
| ESP32-S3 | `docs/firmware/skybridge-esp32s3.bin` |
| ESP32-C3 | `docs/firmware/skybridge-esp32c3.bin` |
| ESP32-C6 | `docs/firmware/skybridge-esp32c6.bin` |
| ESP32-C5 | `docs/firmware/skybridge-esp32c5.bin` |

```bash
python -m esptool --chip esp32c6 --port COM4 erase_flash
python -m esptool --chip esp32c6 --port COM4 write_flash 0x0 docs/firmware/skybridge-esp32c6.bin
```

Die Datei wird immer an Adresse **`0x0`** geschrieben.

## Schritt 4 – SkyBridge einrichten

1. Am Handy mit dem WLAN **`SkyBridge-Setup`** verbinden (Passwort **`skybridge`**).
   <!-- BILD: docs/img/handy-wlan.png -->
2. **`http://192.168.4.1`** öffnen.
3. Unter **„1. Board, WLAN & MQTT“**:
   - **Board** auswählen
   - **WLAN** eintragen (muss **2,4 GHz** sein)
   - **MQTT-Server**: IP-Adresse von Home Assistant, Port `1883`
   - **MQTT-Benutzer/Passwort**: am besten einen eigenen HA-Benutzer anlegen (siehe Schritt 5)
   - Speichern. Die Bridge startet neu und verbindet sich mit deinem WLAN.

   <img src="docs/img/setup-1-wlan-mqtt.png" alt="Board, WLAN und MQTT eintragen" width="320">

4. Die neue **IP-Adresse der Bridge** im Router nachsehen (z. B. Fritzbox: *Heimnetz → Netzwerk*) und im Browser öffnen.
5. Unter **„2. Lampen (Bluetooth Mesh)“**:
   - Die **Export-Datei** aus Schritt 2 auswählen. NetKey, AppKey und die Lampenliste werden automatisch ausgefüllt.
   - Lampen **benennen** (eine pro Zeile: `Adresse Name`, z. B. `0002 Gang vorne`) und fehlende Adressen ergänzen.
   - **Speichern.** Nach etwa einer Minute stehen die Lampen auf der Startseite als erreichbar.

   <img src="docs/img/setup-2-lampen.png" alt="Export-Datei hochladen und Lampen benennen" width="320">

6. Oben auf der Seite zeigt der **Status**, ob alles läuft:

   <img src="docs/img/setup-3-status.png" alt="Statusanzeige" width="320">


> Während der Ersteinrichtung ist Bluetooth absichtlich **aus**. Der ESP hat nur ein Funkteil, und das
> Einrichtungs-WLAN wäre sonst kaum erreichbar.

Der **IV-Index** wird normalerweise automatisch von den Lampen übernommen. Das Feld kannst du leer lassen.

## Schritt 5 – Home Assistant

1. **MQTT** muss eingerichtet sein (*Einstellungen → Geräte & Dienste → MQTT*), z. B. mit dem Add-on *Mosquitto broker*.
2. **MQTT-Benutzer:** *Einstellungen → Personen → Benutzer → Benutzer hinzufügen*, z. B. `skybridge`
   (ohne Administratorrechte). Mosquitto akzeptiert HA-Benutzer automatisch.
   ⚠️ Der Name **`homeassistant`** ist beim Mosquitto-Add-on reserviert und funktioniert nicht.
   <!-- BILD: docs/img/ha-benutzer.png -->
3. Die Lampen erscheinen unter *MQTT → Geräte* als eigene Geräte, dazu **„Alle Lampen“** am Gerät *SkyBridge*.
   <!-- BILD: docs/img/ha-mqtt-geraete.png -->
   <!-- BILD: docs/img/ha-geraet.png -->

**Beispiel-Automation** (Präsenzmelder schaltet Licht):

```yaml
alias: Licht Gang per Bewegung
triggers:
  - trigger: state
    entity_id: binary_sensor.bewegung_gang
actions:
  - choose:
      - conditions:
          - condition: state
            entity_id: binary_sensor.bewegung_gang
            state: "on"
        sequence:
          - action: light.turn_on
            target:
              entity_id: [light.gang_vorne_1, light.gang_vorne_2]
            data:
              brightness_pct: 80
              color_temp_kelvin: 3000
    default:
      - delay: "00:01:00"
      - action: light.turn_off
        target:
          entity_id: [light.gang_vorne_1, light.gang_vorne_2]
mode: restart
```

> Einzelne Lampen bestätigen jeden Befehl und melden ihren echten Zustand. „Alle Lampen“ schickt einen
> gemeinsamen Befehl ohne Rückmeldung. Für Automationen sind die einzelnen Lampen deshalb die zuverlässigere Wahl.

> ⚠️ Hängen die Lampen an einem **Schaltaktor** (z. B. KNX), muss dieser **dauerhaft eingeschaltet** bleiben.
> Ohne Strom können die Lampen keine Funkbefehle empfangen.

---

## Fehlersuche

| Problem | Lösung |
|---|---|
| **„Failed to open serial port“** | Der Anschluss ist belegt. Arduino IDE, VS Code/PlatformIO, 3D-Drucker-Software und andere Tabs schließen, ggf. PC neu starten. |
| Board wird nicht erkannt | BOOT-Taste halten, RESET drücken, BOOT loslassen. Windows: Treiber für CH340/CP210x installieren. Anderes USB-Kabel probieren (manche können nur laden). |
| **„This chip is ESP32-C6, not ESP32-C3“** | Das Board ist anders beschriftet, als es ist. Die Datei für den **erkannten** Chip nehmen (Web-Installer und `flash.py` machen das automatisch). |
| Handy kommt nicht ins `SkyBridge-Setup`-WLAN | Netz „vergessen“ und neu verbinden. Passwort `skybridge`. „Kein Internet“ bestätigen. |
| Lampen erscheinen nicht in HA | Auf der Statusseite prüfen, ob MQTT „verbunden“ ist. MQTT-Server-IP und Benutzer prüfen (nicht `homeassistant`). |
| Lampen „nicht erreichbar“ | Haben die Lampen Strom? Bridge näher an die Lampen. NetKey/AppKey stimmen? Lampen mit der Telink-App testen. |
| WLAN geändert | Reset-Taste **5 s halten**. WLAN/MQTT werden gelöscht, die Lampen-Einrichtung bleibt. Dann wieder mit `SkyBridge-Setup` verbinden. |

**Log ansehen:** Board per USB anschließen und einen seriellen Monitor mit **115200 Baud** öffnen,
z. B. im Web-Installer unter *„Logs & Console“* oder mit `python -m serial.tools.miniterm COM4 115200`.

## Sicherheit

- Im Quellcode stehen **keine Schlüssel und keine Passwörter**. Alles wird auf der Einrichtungsseite eingegeben und nur im ESP gespeichert.
- Die Mesh-Schlüssel werden nach dem Speichern **nie wieder angezeigt**.
- Die Einrichtungsseite hat **kein Passwort**. Jeder in deinem WLAN kann sie öffnen. Gäste gehören daher ins Gäste-WLAN.
- `.gitignore` schließt `*.json` aus, damit die Mesh-Export-Datei nicht versehentlich eingecheckt wird.

---

## Für Entwickler

### Selbst bauen

Benötigt **ESP-IDF v5.4.2**:

```bash
idf.py set-target esp32c6   # esp32 | esp32s3 | esp32c3 | esp32c6 | (esp32c5: idf.py --preview ...)
idf.py build
cd build && python -m esptool --chip esp32c6 merge_bin -o ../skybridge-esp32c6.bin @flash_args
```

Bei jedem Push baut GitHub Actions alle Varianten (`.github/workflows/build.yml`). Bei einem Tag `v*` werden
sie als Release veröffentlicht. Für den Web-Installer die neuen `.bin`-Dateien nach `docs/firmware/` kopieren
und GitHub Pages auf den Ordner `docs/` stellen (*Settings → Pages → Branch `main`, Ordner `/docs`*).

### Aufbau

| Datei | Inhalt |
|---|---|
| `main/mesh.c` | Bluetooth Mesh: Die Bridge läuft als zweiter Provisioner mit den importierten Schlüsseln und sendet Generic OnOff / Light Lightness / Light CTL |
| `main/mqtt_ha.c` | MQTT mit Home-Assistant-Discovery (JSON-Schema) |
| `main/portal.c` | Einstellungen (NVS) und Weboberfläche inkl. Import der Mesh-Export-Datei |
| `main/boards.h` | Board-Profile (Reset-Taste, XIAO-C6-Antenne) |
| `docs/` | Web-Installer (ESP Web Tools), fertige Firmware und Bilder (`docs/img/`) |
| `tools/flash.py` | Flash-Skript mit Chip-Erkennung |
| `apps/android/` | Telink SIG Mesh App (Apache 2.0) |

### MQTT-Themen

| Thema | Inhalt |
|---|---|
| `skybridge/<id>/status` | `online` / `offline` (Last Will) |
| `skybridge/<id>/<adresse>/set` | Befehl, JSON wie HA: `{"state":"ON","brightness":200,"color_temp":3000,"transition":2}` |
| `skybridge/<id>/<adresse>/state` | Zustand |
| `skybridge/<id>/<adresse>/avail` | Erreichbarkeit der Lampe |
| `skybridge/<id>/ffff/set` | alle Lampen |

## Lizenz

SkyBridge steht unter der [MIT-Lizenz](LICENSE). Die mitgelieferte Telink-App steht unter der
[Apache License 2.0](apps/android/LICENSE-Telink-Apache-2.0.txt).

Dieses Projekt ist nicht mit OPPLE Lighting oder Telink Semiconductor verbunden. Alle Marken gehören ihren Inhabern.

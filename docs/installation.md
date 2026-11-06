# Installation – ESP32 Energy Manager

**Projekt:** ESP32 Energy Manager  
**Hardware:** ESP32-2432S028 (CYD, 2,8 Zoll)  
**Dokumentationsstand:** V2.4.2

## 1. Voraussetzungen

Für die Installation werden folgende Komponenten benötigt:

### Hardware

- ESP32-2432S028 (Cheap Yellow Display / CYD)
- USB-Datenkabel zur Programmierung
- MicroSD-Karte zur Datenaufzeichnung
- WLAN mit Zugriff auf die Datenquellen
- Stromzähler-Lesekopf mit Tasmota und SML-Ausgabe
- OpenDTU mit erreichbarer HTTP-API für die PV-Daten

Der ESP32, der Lesekopf und OpenDTU müssen sich über das Netzwerk erreichen können.

### Software

- Arduino IDE 2.x
- ESP32-Boardpaket für die Arduino IDE
- Die im aktuellen Sketch verwendeten Bibliotheken
- Die Projektdateien aus diesem Repository

## 2. Arduino IDE vorbereiten

1. Arduino IDE installieren und starten.
2. Unter **Datei → Voreinstellungen** die zusätzliche Boardverwalter-URL für ESP32 eintragen:

   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`

3. Unter **Werkzeuge → Board → Boardverwalter** nach `esp32` suchen.
4. Das Boardpaket **esp32 by Espressif Systems** installieren.

**Wichtig:** Die genaue, mit V2.4.2 getestete Version des ESP32-Boardpakets wird noch ergänzt.

## 3. Projekt herunterladen

Auf der GitHub-Projektseite:

1. Auf **Code** klicken.
2. **Download ZIP** auswählen.
3. ZIP-Datei entpacken.

Der Arduino-Sketch befindet sich im Verzeichnis:

```text
ESP32_Energy_Monitor/
└── ESP32_Energy_Monitor.ino
```

Weitere zum Sketch gehörende Dateien, insbesondere `cyd_config.h`, müssen im selben Sketch-Ordner liegen, sofern sie vom Code eingebunden werden.

## 4. Benötigte Bibliotheken installieren

Das Projekt verwendet das ESP32-Boardpaket von Espressif sowie drei zusätzliche Arduino-Bibliotheken.

### Zusätzliche Bibliotheken

In der Arduino IDE unter **Sketch → Bibliothek einbinden → Bibliotheken verwalten** folgende Bibliotheken suchen und installieren:

1. **ArduinoJson** – JSON-Daten der Energiequellen verarbeiten
2. **TFT_eSPI** – TFT-Display ansteuern
3. **XPT2046_Touchscreen** – Touchcontroller auslesen

Die weiteren verwendeten Bibliotheken und System-Header sind Bestandteil der ESP32-Entwicklungsumgebung und müssen normalerweise nicht separat installiert werden.

### Displaykonfiguration

Das ESP32-2432S028 (CYD) verwendet typischerweise einen ILI9341-Displaycontroller mit 320 × 240 Pixeln.

**Wichtig:** Die Bibliothek `TFT_eSPI` benötigt eine passende Displaykonfiguration. Die Datei `cyd_config.h` allein konfiguriert `TFT_eSPI` nicht automatisch.

Die erforderlichen Displaypins, SPI-Einstellungen und der Displaytreiber müssen daher in der `TFT_eSPI`-Konfiguration oder über eine entsprechende Build-Konfiguration festgelegt sein.

Die exakte, getestete TFT_eSPI-Konfiguration wird noch dokumentiert.

## 5. Hardware-Pinbelegung

Die Datei `cyd_config.h` enthält folgende Pinbelegung für das verwendete ESP32-CYD-Board.

### Hintergrundbeleuchtung

| Funktion | GPIO |
|---|---|
| TFT Backlight | 21 |

### Touchcontroller XPT2046

| Funktion | GPIO |
|---|---|
| Touch CS | 33 |
| Touch IRQ | 36 |
| Touch CLK | 25 |
| Touch MOSI | 32 |
| Touch MISO | 39 |

### MicroSD-Karte

| Funktion | GPIO |
|---|---|
| SD CS | 5 |
| SD SCK | 18 |
| SD MISO | 19 |
| SD MOSI | 23 |

### Touchkalibrierung

In `cyd_config.h` sind folgende Rohwerte hinterlegt:

```cpp
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3900
#define TOUCH_Y_MIN 200
#define TOUCH_Y_MAX 3900
```

Diese Werte sind typische Ausgangswerte. Je nach Display können eine individuelle Kalibrierung oder Anpassungen der Achsenausrichtung erforderlich sein.

**Hinweis:** ESP32-CYD-Boards existieren in unterschiedlichen Hardware-Revisionen. Die Pinbelegung muss zur tatsächlich verwendeten Platine passen.

## 6. Projektdateien

Die für den Sketch erforderlichen Dateien müssen sich im selben Arduino-Sketch-Verzeichnis befinden:

```text
ESP32_Energy_Monitor/
├── ESP32_Energy_Monitor.ino
└── cyd_config.h
```

Die Datei `cyd_config.h` wird vom Sketch über `#include "cyd_config.h"` eingebunden und muss deshalb beim Kompilieren verfügbar sein.

## 7. Board und Schnittstelle auswählen

1. ESP32-CYD über ein USB-Datenkabel mit dem Computer verbinden.
2. In der Arduino IDE das passende ESP32-Board auswählen.
3. Den richtigen seriellen Port auswählen.
4. Die für das verwendete CYD-Modell erforderlichen Boardeinstellungen prüfen.

**Hinweis:** Die konkreten Boardoptionen werden noch anhand der funktionierenden Entwicklungsumgebung dokumentiert.

## 8. Sketch kompilieren und übertragen

1. `ESP32_Energy_Monitor.ino` in der Arduino IDE öffnen.
2. Prüfen, ob alle benötigten Dateien im Sketch-Ordner vorhanden sind.
3. Mit **Überprüfen** den Sketch kompilieren.
4. Eventuelle fehlende Bibliotheken installieren.
5. Mit **Hochladen** die Firmware auf den ESP32 übertragen.

Nach erfolgreicher Übertragung startet der ESP32 Energy Manager.

Bei Problemen können die Ausgaben des seriellen Monitors zur Fehlersuche verwendet werden.

## 9. WLAN und Datenquellen konfigurieren

Der ESP32 benötigt eine WLAN-Verbindung.

Die konkrete Ersteinrichtung hängt von der Konfigurationslogik der installierten Firmware ab.

### Stromzähler mit Tasmota

Beispiel für die HTTP-Abfrage:

```text
http://LESEKOPF-IP/cm?cmnd=status%208
```

Erwartete Messwerte:

| JSON-Feld | Bedeutung |
|---|---|
| `StatusSNS.SML.Power_curr` | Aktuelle Netzleistung in Watt |
| `StatusSNS.SML.Total_in` | Netzbezug gesamt in kWh |
| `StatusSNS.SML.Total_out` | Netzeinspeisung gesamt in kWh |

Die JSON-Struktur kann vorab im Browser geprüft werden.

### Photovoltaik mit OpenDTU

API-Endpunkt:

```text
http://OPENDTU-IP/api/livedata/status
```

Benötigte Felder:

| JSON-Feld | Bedeutung |
|---|---|
| `total.Power.v` | Aktuelle PV-Leistung in Watt |
| `total.YieldTotal.v` | Gesamtertrag in kWh |
| `total.YieldDay.v` | Tagesertrag in Wh |

Die IP-Adressen müssen zur eigenen Netzwerkkonfiguration passen.

## 10. MicroSD-Karte vorbereiten

Die MicroSD-Karte dient zur langfristigen Speicherung von Messwerten und Fehlerprotokollen.

1. Eine geeignete MicroSD-Karte vorbereiten.
2. Die vom aktuellen Sketch unterstützte Formatierung verwenden.
3. Die Karte in den SD-Steckplatz des CYD einsetzen.
4. Den ESP32 starten.
5. Den SD-Status auf dem Display oder in der Weboberfläche kontrollieren.

Die Messwerte werden in täglichen CSV-Dateien abgelegt, beispielsweise:

```text
2026-11-06.csv
```

Fehlerprotokolle werden im Verzeichnis `logs/` gespeichert.

## 11. Funktion prüfen

Nach der Installation sollten folgende Punkte kontrolliert werden:

- [ ] ESP32 startet ohne dauerhafte Fehlermeldung.
- [ ] Display und Touchfunktion reagieren.
- [ ] WLAN-Verbindung wird aufgebaut.
- [ ] Stromzählerwerte werden empfangen.
- [ ] OpenDTU-Daten werden empfangen.
- [ ] Weboberfläche ist über die ESP32-IP erreichbar.
- [ ] SD-Karte wird erkannt.
- [ ] CSV-Aufzeichnung funktioniert.
- [ ] Systemstatus wird korrekt angezeigt.

## 12. Bekannte Einschränkungen von V2.4.2

Die 24-Stunden-Grafik in der Weboberfläche kann gelegentlich nicht vollständig geladen werden. Der Browser meldet dann möglicherweise einen abgebrochenen Vorgang.

Nach bisherigen Tests bleibt das Gerät dabei weiter bedienbar.

Die Langzeitstabilität wird noch untersucht.

## 13. Fehlerbehebung

**ESP32 wird nicht erkannt:** USB-Datenkabel, Treiber und seriellen Port prüfen.

**Kompilierung schlägt fehl:** Bibliotheken, ESP32-Boardpaket und Sketch-Dateien kontrollieren.

**Keine Stromzählerdaten:** IP-Adresse, Tasmota-HTTP-Endpunkt und JSON-Felder überprüfen.

**Keine PV-Daten:** Erreichbarkeit der OpenDTU und API-Ausgabe prüfen.

**SD-Karte wird nicht erkannt:** Kartenformatierung, Sitz der Karte und Hardwarekonfiguration prüfen.

**Weboberfläche nicht erreichbar:** WLAN-Verbindung und angezeigte ESP32-IP-Adresse kontrollieren.

---

Diese Anleitung wird mit weiteren Tests und bestätigten Bibliotheksversionen vervollständigt.

# ⚡ ESP32 Energy Manager

Ein lokaler Energie-Monitor auf Basis des **ESP32-2432S028 (CYD – Cheap Yellow Display)** mit 2,8-Zoll-Touchdisplay, SD-Karten-Aufzeichnung und responsiver Weboberfläche.

Das Projekt erfasst die aktuellen Stromzählerwerte über einen WLAN-Lesekopf mit **Tasmota SML** und die PV-Erzeugung über **OpenDTU**.

Ziel ist ein kompakter, unabhängiger Energie-Monitor mit langfristiger Datenspeicherung und übersichtlicher Darstellung – **ohne Cloud-Anbindung**.

**Aktueller Entwicklungsstand: v2.5.0** – Display- und WebUI-Überarbeitung abgeschlossen und auf der Zielhardware getestet. Die Langzeitstabilität wird weiterhin im praktischen Betrieb beobachtet.

## ✨ Funktionen

- Aktuelle Netzleistung und PV-Erzeugung in Watt
- Tägliche und gesamte Netz- und PV-Energiewerte
- Zeitpunkte der letzten erfolgreichen Datenabrufe
- WLAN- und Datenquellenstatus sowie SD-Kartenstatus
- Touchdisplay mit zwei Hauptansichten, Dark/Light-Modus und 24h-Grafik
- Responsive Weboberfläche mit vier Registerkarten und Wisch-Navigation auf Smartphones
- 24-Stunden-Leistungshistorie mit RAM-Zwischenspeicherung
- CSV-Aufzeichnung auf MicroSD-Karte
- Systemdiagnose mit SD-Belegung und Heap-Informationen
- Fehlerprotokollierung
- Konfiguration der Datenquellen über die Weboberfläche
- Manueller Neustart mit Bestätigung
- Automatische Wiederherstellung bei längeren Verbindungsproblemen

## 🖥️ Hardware

- ESP32-2432S028 (CYD)
- Integriertes 2,8-Zoll-TFT-Display mit Touchfunktion
- MicroSD-Karte
- WLAN-Verbindung
- Kompatibler WLAN-Stromzähler-Lesekopf mit HTTP-/JSON-Ausgabe (Tasmota SML)
- OpenDTU für die Erfassung der PV-Erzeugung

## 🔌 Datenquellen

### Stromzähler

Ein WLAN-Lesekopf mit Tasmota-SML-Ausgabe stellt die Messwerte über HTTP bereit.

Beispiel:

```text
http://LESEKOPF-IP/cm?cmnd=status%208
```

Verwendete JSON-Felder:

| Feld | Bedeutung |
|---|---|
| `StatusSNS.SML.Power_curr` | Aktuelle Netzleistung (W) |
| `StatusSNS.SML.Total_in` | Netzbezug gesamt (kWh) |
| `StatusSNS.SML.Total_out` | Netzeinspeisung gesamt (kWh) |

### Photovoltaik

Die PV-Daten werden über die OpenDTU-API abgerufen:

```text
http://OPENDTU-IP/api/livedata/status
```

Verwendete JSON-Felder:

| Feld | Bedeutung |
|---|---|
| `total.Power.v` | Aktuelle PV-Leistung (W) |
| `total.YieldTotal.v` | Gesamte PV-Erzeugung (kWh) |
| `total.YieldDay.v` | Tagesertrag der PV-Anlage (kWh) |

Die Datenquellen und die dafür vorgesehenen Einstellungen können über die Weboberfläche konfiguriert werden.

## 📱 Touchdisplay (neu in v2.5.0)

Das Display bietet zwei Hauptansichten:

- **Kompakt:** Größere aktuelle Netz- und PV-Leistungswerte, Tageswerte und kleinere Gesamtwerte.
- **Details:** Ausführlichere Darstellung der Netz- und PV-Daten in zwei Spalten.

**Bedienung:**

| Aktion | Funktion |
|---|---|
| Doppeltippen auf der Hauptansicht | Zwischen Kompakt und Details wechseln |
| Dark/Light-Schalter oben rechts | Displaydesign umschalten |
| Blauer 24h-Button | Leistungsgrafik öffnen |
| Tippen in der Grafik | Zur vorherigen Hauptansicht zurückkehren |
| Titel ca. 3 Sekunden gedrückt halten | Systemmenü öffnen |
| Neustart im Systemmenü | Neustart nach Bestätigung |

Die ausgewählte Hauptansicht und das Displaydesign bleiben nach einem Neustart erhalten. Die 24h-Grafik verwendet gestrichelte Hilfslinien sowie hervorgehobene Achsen und Kurven.

## 💾 Datenspeicherung

Die Messwerte werden auf einer MicroSD-Karte in täglichen CSV-Dateien gespeichert.

Beispiel:

```text
2026-11-06.csv
```

Die Aufzeichnung enthält Zeitstempel, Zählerstände sowie aktuelle Netz- und PV-Leistungen.

Für die 24-Stunden-Darstellung werden historische Daten beim Start in einen RAM-Ringpuffer geladen. Neue Werte ergänzen diesen Puffer während des Betriebs. Dadurch sollen unnötige SD-Zugriffe bei der Darstellung vermieden werden.

## 🌐 Weboberfläche (überarbeitet in v2.5.0)

Die responsive Weboberfläche ist in **vier Registerkarten** gegliedert:

| Registerkarte | Inhalt |
|---|---|
| **Übersicht** | Live-Leistungswerte, hervorgehobene Tageswerte, Gesamtstände und Status von WLAN, Lesekopf, Solarzugriff und SD-Karte |
| **24h-Verlauf** | Leistungsgrafik der letzten 24 Stunden |
| **System** | Verbindungsdiagnose, SD-Kartenbelegung, Heap-Speicher und Fehlerprotokoll |
| **Einstellungen** | Getrennte Bereiche für Lesekopf, Solardaten und allgemeine Einstellungen |

Weitere Bedienfunktionen:

- **Dark/Light-Modus**, dessen Auswahl im Browser gespeichert wird
- **Wischen nach links/rechts** über den Seiteninhalt zum Wechseln der Registerkarte auf Smartphones
- Horizontal verschiebbare Registerleiste bei schmalen Displays
- Weiterhin direkte Auswahl der Registerkarten durch Antippen
- Neustartfunktion mit Sicherheitsabfrage

### Statusfarben und SD-Warnschwellen

- **Grün:** Verbindung bzw. Funktion in Ordnung
- **Rot:** Verbindung oder Funktion gestört
- **SD-Belegung unter 80 %:** Grün
- **SD-Belegung ab 80 % bis unter 90 %:** Gelb (Warnung)
- **SD-Belegung ab 90 %:** Rot (kritisch)
- **SD-Karte nicht verfügbar / Fehler:** Rot

### Schonender Umgang mit Ressourcen

Die Weboberfläche lädt umfangreichere Informationen wie 24h-Verlauf, Fehlerprotokoll und Einstellungen bedarfsgesteuert. Nicht benötigte regelmäßige Browser-Abfragen werden vermieden. Die eigentliche Messwerterfassung und SD-Aufzeichnung laufen unabhängig von der geöffneten Registerkarte weiter.

## 🚀 Installation

1. Arduino IDE mit ESP32-Boardunterstützung einrichten.
2. Benötigte Bibliotheken installieren.
3. `ESP32_Energy_Monitor.ino` zusammen mit `cyd_config.h` im Sketch-Ordner ablegen.
4. Passende ESP32-CYD-Boardkonfiguration und TFT_eSPI-Einstellungen auswählen.
5. Sketch kompilieren und auf den ESP32 übertragen.
6. WLAN und Datenquellen über die vorgesehenen Konfigurationsmöglichkeiten einrichten.
7. MicroSD-Karte einsetzen und die Aufzeichnung prüfen.

**Wichtig:** Im Sketch-Ordner darf nur **eine vollständige `.ino`-Version** liegen. Die Arduino IDE kompiliert alle `.ino`-Dateien eines Sketch-Ordners gemeinsam. Ältere Versionen daher in separaten Sicherungsordnern aufbewahren.

**Hinweis:** Die genauen Bibliotheksversionen und Boardeinstellungen sind noch nicht vollständig dokumentiert. Die Konfiguration hängt von der verwendeten CYD-Boardrevision ab.

## 📌 Entwicklungsstand und Versionshinweise

### v2.5.0 – Display und WebUI

- Neue kompakte und detaillierte Touchdisplayansicht
- Verbesserte Lesbarkeit und Touchbedienung
- Dark/Light-Modus auf Display und Weboberfläche
- Überarbeitete 24h-Grafik auf dem Display
- Vier Web-Registerkarten mit Smartphone-Wischgesten
- Hervorgehobene Tageswerte und kompakte Statusanzeigen
- Erweiterte Systemansicht mit SD- und Heap-Diagnose
- Neu strukturierte Einstellungen
- Bedarfsgesteuertes Laden der Webdaten

Die Funktionen wurden auf der Zielhardware praktisch getestet. Ein längerer Stabilitätstest läuft noch.

### v2.4.2 – Vorherige Version

- Optimierungen der Weboberfläche und Grafikabfragen
- RAM-basierte Leistungshistorie
- Erweiterte Neustart- und Wiederherstellungsfunktionen
- Speicher- und Systemdiagnose

### Beobachtungspunkte

Bei einer früheren Version wurde gelegentlich ein Abbruch beim Laden der **24h-Grafik im Browser** beobachtet. Ob dieses Verhalten unter v2.5.0 weiterhin auftritt, ist noch nicht abschließend geklärt.

Auch die Langzeitstabilität, der freie Heap-Speicher und die Vollständigkeit der CSV-Aufzeichnung werden im praktischen Betrieb weiter beobachtet.

## 🛠️ Mögliche spätere Entwicklung

- Weitergehende Untersuchung und gegebenenfalls Stabilisierung der 24h-Grafikübertragung
- Interaktive Historiengrafiken mit längeren Zeiträumen (z. B. Tage, Wochen, Monate)
- Ressourcenschonende Datenaggregation und bedarfsgesteuerte Übertragung für längere Zeiträume
- Weitere Ergänzungen der Installations- und Hardwaredokumentation

Diese Punkte sind Ideen für spätere Versionen, **kein Bestandteil von v2.5.0**.

## 📄 Lizenz

Eine Lizenz wurde noch nicht festgelegt.

## 🤝 Projektstatus

Dieses Projekt befindet sich in aktiver Entwicklung und wird auf echter ESP32-CYD-Hardware getestet.

Feedback, Verbesserungsvorschläge und Erfahrungen sind willkommen.

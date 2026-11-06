# ⚡ ESP32 Energy Manager

Ein lokaler Energie-Monitor auf Basis des **ESP32-2432S028 (CYD – Cheap Yellow Display)** mit 2,8-Zoll-Touchdisplay, SD-Karten-Aufzeichnung und responsiver Weboberfläche.

Das Projekt erfasst den aktuellen Stromverbrauch über einen WLAN-Lesekopf am Stromzähler und die PV-Erzeugung über **OpenDTU**.

Ziel ist ein kompakter, unabhängiger Energie-Monitor mit langfristiger Datenspeicherung und übersichtlicher Darstellung – ohne Cloud-Anbindung.

## ✨ Funktionen

- Anzeige der aktuellen Netzleistung und PV-Erzeugung in Watt
- Anzeige des täglichen und gesamten Netzbezugs
- Anzeige der täglichen und gesamten PV-Erzeugung
- Uhrzeit des letzten erfolgreichen Datenabrufs für beide Quellen
- WLAN-Status und Signalstärke
- SD-Kartenstatus und Speicherbelegung
- Touchdisplay mit Dark- und Light-Mode
- Responsive Weboberfläche für Smartphone und PC
- 24-Stunden-Leistungshistorie mit RAM-Zwischenspeicherung
- CSV-Aufzeichnung auf SD-Karte
- Fehlerprotokollierung
- Konfiguration der Datenquellen über die Weboberfläche
- Manueller Neustart über Weboberfläche und Display
- Automatische Wiederherstellung bei längeren Verbindungsproblemen

## 🖥️ Hardware

- ESP32-2432S028 (CYD)
- Integriertes 2,8-Zoll-TFT-Display mit Touchfunktion
- MicroSD-Karte
- WLAN-Verbindung
- Kompatibler WLAN-Stromzähler-Lesekopf mit HTTP-/JSON-Ausgabe
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

Die Datenquellen und Abfrageintervalle können über die Weboberfläche konfiguriert werden.

## 💾 Datenspeicherung

Die Messwerte werden auf einer MicroSD-Karte in täglichen CSV-Dateien gespeichert.

Beispiel:

```text
2026-11-06.csv
```

Die Aufzeichnung enthält Zeitstempel, Zählerstände sowie aktuelle Netz- und PV-Leistungen.

Für die 24-Stunden-Darstellung werden historische Daten beim Start in einen RAM-Ringpuffer geladen. Neue Werte ergänzen diesen Puffer während des Betriebs.

Dadurch soll die Weboberfläche möglichst wenig auf die SD-Karte zugreifen müssen.

## 🌐 Weboberfläche

Die Weboberfläche bietet:

- Live-Dashboard im Stil der Displayanzeige
- Konfiguration der Datenquellen
- Systemstatus und Speicherdiagnose
- Fehlerprotokolle
- 24-Stunden-Leistungsgrafik
- Manuellen Neustart mit Sicherheitsabfrage

Die Oberfläche ist für Desktop-Browser und Smartphones ausgelegt.

## 🚀 Installation

1. Arduino IDE mit ESP32-Boardunterstützung einrichten.
2. Benötigte Bibliotheken installieren.
3. Projekt-Sketch öffnen.
4. Passende ESP32-CYD-Boardkonfiguration auswählen.
5. Sketch kompilieren und auf den ESP32 übertragen.
6. WLAN und Datenquellen über die vorgesehenen Konfigurationsmöglichkeiten einrichten.
7. MicroSD-Karte einsetzen und die Aufzeichnung prüfen.

**Hinweis:** Die genauen Bibliotheksversionen und Boardeinstellungen werden noch dokumentiert.

## 📌 Entwicklungsstand

**Aktuelle Testversion: V2.4.2**

Diese Version enthält insbesondere:

- Optimierte Weboberfläche
- Reduzierte Blockierung bei Grafikabfragen
- RAM-basierte Leistungshistorie
- Erweiterte Neustart- und Wiederherstellungsfunktionen
- Speicher- und Systemdiagnose

### Bekannte Einschränkungen

Die 24-Stunden-Grafik im Browser wird derzeit nicht immer zuverlässig geladen. Die Anfrage kann mit einer Abbruchmeldung enden.

Das Öffnen der Grafik blockiert in V2.4.2 nach bisherigen Tests nicht mehr das gesamte System.

Die Langzeitstabilität wird derzeit im praktischen Betrieb untersucht.

## 🛠️ Geplante Entwicklung

- Weitere Stabilitätsverbesserungen der Grafikübertragung
- Langzeittests und Speicheroptimierung
- Verbesserung der Projektdokumentation
- Zusätzliche Energieauswertungen in späteren Versionen

## 📄 Lizenz

Eine Lizenz wurde noch nicht festgelegt.

## 🤝 Projektstatus

Dieses Projekt befindet sich in aktiver Entwicklung und wird auf echter ESP32-CYD-Hardware getestet.

Feedback, Verbesserungsvorschläge und Erfahrungen sind willkommen.

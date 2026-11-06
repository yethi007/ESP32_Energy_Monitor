# Changelog – ESP32 Energy Manager

Alle wichtigen Änderungen am ESP32 Energy Manager werden in dieser Datei dokumentiert.

Das Projekt befindet sich in aktiver Entwicklung. Die Versionsangaben beschreiben Entwicklungsstände und sind nicht automatisch als offizielle GitHub-Releases zu verstehen.

## [Unreleased]

### Geplant

- Zuverlässigere Übertragung und Darstellung der 24-Stunden-Grafik im Browser
- Weitere Langzeit- und Stabilitätstests
- Optimierung der Speicher- und Netzwerkverarbeitung
- Verbesserung der Dokumentation und Installationsanleitung
- Konzept für interaktive Grafiken über längere Zeiträume (z. B. 7 Tage, 30 Tage) mit ressourcenschonender Datenaufbereitung

## [2.5.0] – Display- und WebUI-Überarbeitung

### Touchdisplay (V2.5.0-A)

- Kompakte und detaillierte Hauptansicht mit Wechsel per Doppeltipp
- Größere Anzeige der aktuellen Netz- und PV-Leistung
- Dark-/Light-Umschaltung und Speicherung der gewählten Ansicht und Darstellung
- Überarbeitete 24-Stunden-Grafik mit gestrichelten Hilfslinien, kräftigeren Kurven und durchgezogenen Achsen
- Farbige Statusanzeigen und überarbeiteter 24h-Grafik-Button
- Systemmenü mit Neustartbestätigung per längerem Drücken

### Weboberfläche (V2.5.0-B)

- Neues responsives Layout mit den vier Registerkarten Übersicht, 24h-Verlauf, System und Einstellungen
- Horizontale Registerleiste und Wisch-Navigation zwischen den Seiten auf Smartphones
- Dark-/Light-Umschaltung mit Speicherung der Auswahl im Browser
- Größer hervorgehobene Tagesenergiewerte und Statusanzeigen für WLAN, Lesekopf, Solarzugriff und SD-Karte
- Systemansicht mit detailliertem Verbindungsstatus, SD-Speicherbelegung, Heap-Diagnose und Fehlerprotokoll
- SD-Füllstandsanzeige: grün unter 80 %, gelb ab 80 %, rot ab 90 %; Fehler rot
- Neu strukturierte Einstellungen für Lesekopf, Solardaten und allgemeine Optionen
- Bedarfsgesteuertes Nachladen von Verlauf, Diagnose und Einstellungen statt unnötiger Hintergrundabfragen

### Hinweise

- Display und Weboberfläche wurden auf der Zielhardware im praktischen Betrieb getestet.
- Die Langzeitstabilität von V2.5.0 wird weiter beobachtet.
- Der zuvor gelegentlich beobachtete Abbruch der 24-Stunden-Grafik im Browser bleibt ein Beobachtungspunkt; eine endgültige Behebung ist nicht bestätigt.

## [2.4.2] – Vorheriger Entwicklungsstand

### Hinzugefügt

- Manueller Neustart über die Systemstatus-Seite der Weboberfläche
- Neustartfunktion über längere Touch-Betätigung auf der Display-Systemseite
- Automatische Wiederherstellungslogik bei längerem Ausfall beider Datenquellen
- Schutz vor zu häufigen automatischen Neustarts
- Erweiterte Watchdog-Überwachung

### Verbessert

- Übertragung der Web-Grafik in kleineren Datenblöcken
- Reaktionsfähigkeit der Weboberfläche während größerer HTTP-Antworten
- Stabilität bei wiederholten Grafikabfragen
- Bedienbarkeit des Systems bei fehlgeschlagenen Browseranfragen

### Bekannte Probleme

- Die 24-Stunden-Grafik wird im Browser gelegentlich nicht vollständig geladen.
- Browser können dabei eine Meldung wie „The operation was aborted“ anzeigen.
- Die Langzeitstabilität wird weiterhin im praktischen Betrieb getestet.

## [2.4.1]

### Verbessert

- Interne Organisation globaler Variablen
- Strukturierung und Kommentierung des Programmcodes
- Aktualisierungsverhalten der Weboberfläche
- Systemdiagnose und Anzeige von Speicherinformationen

### Hinzugefügt

- Zusätzliche Heap-Diagnoseinformationen über die Systemstatus-API

## Frühere Entwicklungsstände

Die folgenden Funktionen wurden bereits vor V2.4.1 entwickelt. Ihre genaue Zuordnung zu einzelnen Versionen ist noch nicht vollständig dokumentiert.

### Grundfunktionen

- Auslesen eines Tasmota-SML-Stromzählers über HTTP
- Abfrage der PV-Leistung und Ertragswerte über OpenDTU
- Anzeige von Netzleistung und PV-Erzeugung auf dem CYD-Touchdisplay
- Darstellung von Tages- und Gesamtenergiewerten
- Anzeige des letzten erfolgreichen Datenabrufs
- WLAN-Status mit Signalstärke und IP-Adresse

### Weboberfläche

- Browser-Dashboard für aktuelle Messwerte
- Konfigurationsmöglichkeiten für Datenquellen
- Systemstatusseite
- Fehlerprotokollanzeige
- 24-Stunden-Leistungsdiagramm
- Dark- und Light-Mode

### Datenspeicherung

- CSV-Aufzeichnung auf MicroSD-Karte
- Tägliche Messwertdateien
- Fehlerprotokolle im Verzeichnis `logs/`
- Konfigurierbare Aufbewahrungsdauer
- RAM-Ringpuffer für historische Leistungswerte

## Hinweise zur Versionshistorie

Die bisherige Entwicklung erfolgte zunächst als fortlaufende Test- und Verbesserungsarbeit.

Mit der Veröffentlichung auf GitHub sollen zukünftige Änderungen regelmäßig in diesem Changelog dokumentiert werden.

Neue Versionen erhalten möglichst einen passenden Git-Tag und gegebenenfalls ein GitHub-Release.

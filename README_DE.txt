ENERGY MANAGER V2.4 – STABILITAET / RAM-HISTORIE
================================================

NEU
- Web-Startseite / laedt nur Live-Status, kein Diagramm.
- Separate Web-Grafik unter /chart; nur diese Seite ruft /api/chart24 ab.
- 24h-Historie liegt im RAM (maximal 1440 Messpunkte, ca. 17 KB).
- Nach Neustart werden die CSV-Dateien von gestern und heute EINMALIG
  in kleinen Portionen (12 Zeilen pro loop) eingelesen.
- Jede erfolgreich geschriebene Minuten-CSV-Zeile wird anschliessend
  auch in die RAM-Historie uebernommen. Web und TFT lesen nur RAM.
- Web-Diagramm liefert maximal 288 gemittelte Datenpunkte.
- Web-Live-Status wird alle 2 Sekunden abgefragt.
- SD-Fuellstand wird nur alle 10 Minuten ermittelt.
- Display: Live-Hauptseite und separate 24h-Grafik durch Tippen.

WICHTIG
- Alle bestehenden CSV-Dateien und Einstellungen bleiben erhalten.
- Vor dem Flashen SD-Karte sichern. Die erste Grafik kann nach dem
  Start kurz unvollstaendig sein, bis die Historie eingelesen ist.
- Falls die SD-Karte fehlt, gibt es keine persistente Historie.
- Der Sketch ist NICHT auf echter CYD-Hardware kompiliert/getestet.
- Bei weiterhin auftretenden Haengern bitte Dauer, Web-Seite und
  Seriell-Monitor-Ausgabe dokumentieren.

ENERGY MANAGER V2.3 — CYD ESP32-2432S028
=======================================

Neuerungen gegenüber V2.2:
- Neue kompakte Live-Anzeige im Querformat (320x240), Titel Energy Manager mit gezeichnetem Sonne/Blitz-Logo.
- Neben Netz und PV steht jeweils die Uhrzeit des letzten erfolgreichen HTTP-Abrufs.
- Abrufzeit gelb ab >30 Sekunden, rot ab >60 Sekunden ohne Erfolg.
- Netzbezug gesamt und PV-Erzeugung gesamt sowie beide Tageswerte auf dem Display.
- WLAN-Status, Signalstärke, SD-Status, SD-Belegung in Prozent und IP-Adresse.
- Dark-/Light-Mode und 24-Stunden-Grafik bleiben erhalten.
- SD-Belegung wird maximal einmal pro Minute aktualisiert.

Hinweis: Die SD-Belegung zeigt den belegten Anteil der gesamten Karte, nicht nur
die CSV-Dateien. Bei nicht verfügbarer SD-Karte steht SD: Fehler.

Die Messdaten-CSV, die Web-UI und das 30-Tage-Fehlerprotokoll entsprechen V2.2.
Vor dem Flashen die SD-Karte sichern. Nicht auf CYD-Hardware kompiliert/getestet.

ESP32 ENERGY MONITOR V2.2 – ESP32-2432S028 (CYD)

FUNKTIONEN
- 320x240 Querformat; Tippen: Live -> 24h-Grafik -> Dark/Light umschalten (kehrt zur Live-Seite zurueck).
- Der Displaymodus wird dauerhaft in NVS/Preferences gespeichert.
- Web-UI mit 24h-Grafik fuer Netzleistung und optionale PV-Leistung.
- Netzbezug heute und PV-Erzeugung heute auf Display und Web-Startseite.
- Konfigurierbare HTTP/JSON-Lesekopf- und Solarquelle, eigene Abfrageintervalle.
- Ein neues CSV pro Kalendertag mit Strom- und Solardaten zusammen.
- Ein Datensatz pro Minute, Timestamp yyyy-mm-dd-hh-mm-ss.
- Historie einstellbar (Standard 1825 Tage).

CSV: /YYYY-MM-DD.csv
Spalten:
timestamp,import_kwh,export_kwh,net_power_w,solar_power_w,solar_total_kwh
Beispiel:
2026-11-05-19-34-00,7218.63000,239.76700,986.00,1250.00,5432.10000

WICHTIG BEIM UPDATE
- Vor dem Flashen die SD-Karte sichern.
- V2.2 nutzt ein neues CSV-Format. Eine bestehende Tagesdatei mit altem Header
  wird beim ersten Schreiben als YYYY-MM-DD.csv.legacy.bak gesichert.
- Alte /v2_YYYY-MM-DD.csv werden nicht mehr beschrieben und nicht automatisch
  eingelesen; sie koennen nach Sicherung manuell entfernt werden.
- Die neue 24h-Grafik fuellt sich mit den neuen Datensaetzen schrittweise.
- Die Tageswerte werden aus dem ersten gueltigen Zaehlerstand des Tages in der
  neuen CSV berechnet. Ist der ESP32 erst spaeter gestartet, fehlen die Stunden
  davor; Tageswerte sind dann nur seit der ersten Messung des Tages korrekt.
- Bei fehlendem PV-Gesamtzaehler wird die Tageserzeugung als '--' dargestellt.
- 'Netzbezug heute' bedeutet bezogene Energie vom Stromnetz, NICHT den
  gesamten Haushaltsverbrauch inklusive direkt genutzter Solarenergie.
- Netzleistung aus Tasmota Power_curr ist ohne Richtungserkennung ggf. nicht
  eindeutig als Bezug oder Einspeisung zu interpretieren.

TASMOTA SML (vorkonfiguriert fuer neue Installationen)
Host: 192.168.178.29
Port: 80
Pfad: /cm?cmnd=status%208
Import: StatusSNS.SML.Total_in
Export: StatusSNS.SML.Total_out
Momentanleistung: StatusSNS.SML.Power_curr

ARDUINO IDE
1. ZIP entpacken, ESP32_Energy_Monitor.ino oeffnen.
2. Board: ESP32-2432S028, mit derselben TFT_eSPI-Konfiguration wie V1/V2.
3. Benoetigt: ArduinoJson 7.x, TFT_eSPI, XPT2046_Touchscreen,
   ESP32-Arduino-Core sowie mitgelieferte cyd_config.h.
4. Auf ESP32 flashen, Web-UI ueber ESP-IP aufrufen.
5. /config: Abfrageintervalle, Historie, Solar-IP, Pfade und Zugangsdaten.

EINSCHRAENKUNGEN
- Nicht auf echter CYD-Hardware kompiliert oder getestet.
- SD, Touch-Ausrichtung und TFT_eSPI-Konfiguration sind hardwareabhaengig.
- Die Grafik liest maximal 1440 Minutenwerte aus den letzten 24 Stunden.
- Synchrones HTTP-Polling kann bei Zeitueberschreitung blockieren.
- Web-UI hat keine Anmeldung: nur in vertrauenswuerdigen LANs betreiben.
- Die Zeitzone muss per NTP richtig gesetzt sein (Europe/Berlin).

NEU IN V2.2: STABILITAET UND FEHLERPROTOKOLL
- WLAN-Neuverbindung alle 15 Sekunden bei Abbruch; Status mit IP und RSSI in Web-UI.
- Lesekopf und Solar: kurze HTTP-Verbindungs- und Antwort-Timeouts.
- Fehlerhafte HTTP-/JSON-Abfragen verursachen keinen beabsichtigten Neustart.
- Fehlermeldungen mit Zeitstempel auf der SD-Karte im Ordner /logs/.
- Eine Protokolldatei je Tag: /logs/YYYY-MM-DD.log.
- Aufbewahrung: aktueller Tag plus 29 vorherige Tage, automatische Bereinigung stuendlich.
- Wiederholte identische Fehler pro Quelle maximal alle zehn Minuten protokolliert.
- Logdateien werden ab 256 KiB je Tag nicht weiter beschrieben.
- Web: http://ESP-IP/logs zum Ansehen und Herunterladen eines Tagesprotokolls.
- /api/logs?date=YYYY-MM-DD liefert eine einzelne Tages-Logdatei.
- /api/status zeigt WLAN-RSSI, letzte erfolgreiche Abfragen und letzte Fehler je Quelle.
- 24h-Grafik wird im Hauptprogramm alle 5 Minuten neu aus den CSVs eingelesen.
  Webanfragen greifen auf den Cache zu, statt CSV-Dateien einzulesen.
- /api/test plant eine zeitnahe Lesekopf-Abfrage ein (HTTP 202) statt sie
  blockierend innerhalb der Webanfrage auszufuehren.

HINWEISE
- Die bestehenden Messdaten-CSV-Dateien bleiben unveraendert.
- Ereignisse vor erfolgreicher NTP-Zeitsynchronisation koennen nur im seriellen
  Monitor erscheinen; ohne gueltige Uhrzeit ist kein SD-Zeitstempel moeglich.
- Der WebServer und HTTPClient laufen weiterhin in einem Arduino-Loop.
  Einzelne HTTP-Abfragen koennen daher kurzzeitig blockieren (max. etwa 1 Sekunde
  pro Datenquelle, zzgl. DNS und SD-I/O). Eine vollstaendig asynchrone Architektur
  ist noch nicht implementiert.
- Die Log-Webseite hat keine Benutzeranmeldung. Nur in einem vertrauenswuerdigen
  lokalen Netzwerk verwenden und nicht ins Internet freigeben.
- Auf dem CYD und mit der verwendeten ESP32-Core-Version muss der Sketch
  kompiliert und getestet werden. Die Hardware wurde hier nicht getestet.

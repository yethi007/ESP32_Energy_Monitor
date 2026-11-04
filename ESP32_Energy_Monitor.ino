/*
  ESP32 Energy Monitor V1.0 - ESP32-2432S028 (CYD)
  Features:
  - HTTP/JSON meter-head polling (1 s default)
  - Configurable JSON paths for import/export counters and power
  - Manual configuration + mDNS discovery + limited subnet HTTP probe
  - Local web UI and JSON API
  - SD CSV logging once per minute, 365-day retention
  - TFT + XPT2046 touch basic live/config navigation

  Tested target assumptions:
  - ESP32 Arduino core 2.x
  - ILI9341 TFT_eSPI setup (see README)
  - XPT2046_Touchscreen by Paul Stoffregen
  - ArduinoJson 7.x
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "cyd_config.h"

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
SPIClass sdSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
WebServer server(80);
Preferences prefs;

struct Settings {
  String wifiSsid, wifiPass;
  String readerHost = "192.168.1.50";
  uint16_t readerPort = 80;
  String readerPath = "/data";
  String readerUser = "";
  String readerPass = "";
  String importCounterPath = "1.8.0";
  String exportCounterPath = "2.8.0";
  String importPowerPath = "1.7.0";
  String exportPowerPath = "2.7.0";
  String signedPowerPath = "16.7.0";
  bool useSignedPower = false;
  bool signedExportNegative = true;
  uint16_t pollSeconds = 1;
} cfg;

struct MeterData {
  double importKWh = NAN;
  double exportKWh = NAN;
  double importW = NAN;
  double exportW = NAN;
  time_t updated = 0;
  bool valid = false;
} liveData;

uint32_t lastPollMs = 0, lastLogMs = 0, lastDrawMs = 0;
double importPowerSum = 0, exportPowerSum = 0;
uint32_t sampleCount = 0;
double importPowerMax = 0, exportPowerMax = 0;
bool sdReady = false, readerOnline = false;
String lastError = "Noch keine Abfrage";
int screenPage = 0;
uint32_t lastTouchMs = 0;

String jsonEscape(const String &s) {
  String o; for (size_t i=0;i<s.length();i++) {
    char c=s[i]; if(c=='"' || c=='\\') o+='\\';
    if(c=='\n') o+="\\n"; else if(c!='\r') o+=c;
  } return o;
}

String makeReaderUrl() {
  String host = cfg.readerHost;
  if (host.startsWith("http://")) host.remove(0, 7);
  if (host.startsWith("https://")) host.remove(0, 8);
  String url = "http://" + host;
  if (cfg.readerPort != 80) url += ":" + String(cfg.readerPort);
  if (!cfg.readerPath.startsWith("/")) url += "/";
  url += cfg.readerPath;
  return url;
}

void loadSettings() {
  prefs.begin("energy", true);
  cfg.wifiSsid = prefs.getString("ssid", "");
  cfg.wifiPass = prefs.getString("wpass", "");
  cfg.readerHost = prefs.getString("host", cfg.readerHost);
  cfg.readerPort = prefs.getUShort("port", cfg.readerPort);
  cfg.readerPath = prefs.getString("path", cfg.readerPath);
  cfg.readerUser = prefs.getString("user", "");
  cfg.readerPass = prefs.getString("rpass", "");
  cfg.importCounterPath = prefs.getString("ic", cfg.importCounterPath);
  cfg.exportCounterPath = prefs.getString("ec", cfg.exportCounterPath);
  cfg.importPowerPath = prefs.getString("ip", cfg.importPowerPath);
  cfg.exportPowerPath = prefs.getString("ep", cfg.exportPowerPath);
  cfg.signedPowerPath = prefs.getString("sp", cfg.signedPowerPath);
  cfg.useSignedPower = prefs.getBool("signed", false);
  cfg.signedExportNegative = prefs.getBool("neg", true);
  cfg.pollSeconds = prefs.getUShort("poll", 1);
  prefs.end();
}

void saveSettings() {
  prefs.begin("energy", false);
  prefs.putString("ssid", cfg.wifiSsid); prefs.putString("wpass", cfg.wifiPass);
  prefs.putString("host", cfg.readerHost); prefs.putUShort("port", cfg.readerPort);
  prefs.putString("path", cfg.readerPath); prefs.putString("user", cfg.readerUser);
  prefs.putString("rpass", cfg.readerPass);
  prefs.putString("ic", cfg.importCounterPath); prefs.putString("ec", cfg.exportCounterPath);
  prefs.putString("ip", cfg.importPowerPath); prefs.putString("ep", cfg.exportPowerPath);
  prefs.putString("sp", cfg.signedPowerPath); prefs.putBool("signed", cfg.useSignedPower);
  prefs.putBool("neg", cfg.signedExportNegative); prefs.putUShort("poll", cfg.pollSeconds);
  prefs.end();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  if (cfg.wifiSsid.length()) {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-start < 15000) delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("EnergyMonitor-Setup", "energy1234");
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.google.com");
    MDNS.begin("energy-monitor");
    MDNS.addService("http", "tcp", 80);
  }
}

bool getPath(JsonDocument &doc, const String &path, double &out) {
  if (!path.length()) return false;
  JsonVariantConst v = doc.as<JsonVariantConst>();
  int start=0;
  while (start < path.length()) {
    int dot=path.indexOf('.', start);
    String token = dot < 0 ? path.substring(start) : path.substring(start, dot);
    if (v.is<JsonObjectConst>()) v = v[token];
    else if (v.is<JsonArrayConst>()) {
      int idx=token.toInt(); v=v[idx];
    } else return false;
    if (dot < 0) break;
    start=dot+1;
  }
  if (v.is<double>() || v.is<long>() || v.is<int>()) { out=v.as<double>(); return true; }
  if (v.is<const char*>()) { out=String(v.as<const char*>()).toDouble(); return true; }
  return false;
}

bool fetchReader() {
  if (WiFi.status() != WL_CONNECTED) { lastError="WLAN nicht verbunden"; readerOnline=false; return false; }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(1800);
  String url=makeReaderUrl();
  if (!http.begin(client, url)) { lastError="HTTP konnte nicht gestartet werden"; return false; }
  if (cfg.readerUser.length()) http.setAuthorization(cfg.readerUser.c_str(), cfg.readerPass.c_str());
  int code=http.GET();
  if (code != HTTP_CODE_OK) {
    lastError="HTTP-Fehler: "+String(code); http.end(); readerOnline=false; return false;
  }
  String payload=http.getString(); http.end();
  JsonDocument doc;
  DeserializationError err=deserializeJson(doc, payload);
  if (err) { lastError="JSON-Fehler: "+String(err.c_str()); readerOnline=false; return false; }

  double v;
  bool gotI=getPath(doc,cfg.importCounterPath,v); if(gotI) liveData.importKWh=v;
  bool gotE=getPath(doc,cfg.exportCounterPath,v); if(gotE) liveData.exportKWh=v;
  bool gotIP=getPath(doc,cfg.importPowerPath,v); if(gotIP) liveData.importW=max(0.0,v);
  bool gotEP=getPath(doc,cfg.exportPowerPath,v); if(gotEP) liveData.exportW=max(0.0,v);
  if (cfg.useSignedPower && getPath(doc,cfg.signedPowerPath,v)) {
    if (cfg.signedExportNegative) { liveData.importW=max(0.0,v); liveData.exportW=max(0.0,-v); }
    else { liveData.importW=max(0.0,-v); liveData.exportW=max(0.0,v); }
    gotIP=gotEP=true;
  }
  liveData.updated=time(nullptr);
  liveData.valid=gotI || gotE || gotIP || gotEP;
  readerOnline=liveData.valid;
  lastError=liveData.valid ? "" : "JSON empfangen, aber keine konfigurierten Felder gefunden";
  return liveData.valid;
}

String timestampString(time_t t) {
  if (t < 100000) return "Zeit-nicht-synchron";
  struct tm tmv; localtime_r(&t,&tmv); char b[24];
  strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&tmv); return String(b);
}

void pruneOldFiles() {
  if (!sdReady || time(nullptr)<100000) return;
  time_t cutoff=time(nullptr)-365L*24L*60L*60L;
  File dir=SD.open("/"); if(!dir) return;
  File f=dir.openNextFile();
  while(f) {
    String name=String(f.name());
    bool isCsv=name.endsWith(".csv");
    f.close();
    if(isCsv && name.length()>=11) {
      // Daily files are /YYYY-MM-DD.csv; use filename date for retention.
      struct tm tmv={0}; int y=0,m=0,d=0;
      if(sscanf(name.c_str(),"/%d-%d-%d.csv",&y,&m,&d)==3) {
        tmv.tm_year=y-1900; tmv.tm_mon=m-1; tmv.tm_mday=d;
        time_t ft=mktime(&tmv); if(ft<cutoff) SD.remove(name);
      }
    }
    f=dir.openNextFile();
  }
  dir.close();
}

void logMinute() {
  if(!sdReady || time(nullptr)<100000 || !liveData.valid || sampleCount==0) return;
  struct tm tmv; time_t now=time(nullptr); localtime_r(&now,&tmv);
  char path[24]; strftime(path,sizeof(path),"/%Y-%m-%d.csv",&tmv);
  bool exists=SD.exists(path);
  File f=SD.open(path, FILE_APPEND);
  if(!f) { lastError="SD-Schreibfehler"; return; }
  if(!exists) f.println("timestamp,import_kwh,export_kwh,avg_import_w,avg_export_w,max_import_w,max_export_w");
  f.printf("%lu,%.5f,%.5f,%.2f,%.2f,%.2f,%.2f\n",
    (unsigned long)now, liveData.importKWh, liveData.exportKWh,
    importPowerSum/sampleCount, exportPowerSum/sampleCount,
    importPowerMax, exportPowerMax);
  f.close();
  importPowerSum=exportPowerSum=0; sampleCount=0; importPowerMax=exportPowerMax=0;
  static int dayOfYear=-1;
  if(tmv.tm_yday!=dayOfYear) { dayOfYear=tmv.tm_yday; pruneOldFiles(); }
}

String statusJson() {
  String s="{";
  s += "\"online\":" + String(readerOnline?"true":"false");
  s += ",\"wifi\":" + String(WiFi.status()==WL_CONNECTED?"true":"false");
  s += ",\"ip\":\""+WiFi.localIP().toString()+"\"";
  s += ",\"import_kwh\":"+String(liveData.importKWh,3);
  s += ",\"export_kwh\":"+String(liveData.exportKWh,3);
  s += ",\"import_w\":"+String(liveData.importW,1);
  s += ",\"export_w\":"+String(liveData.exportW,1);
  s += ",\"updated\":\""+timestampString(liveData.updated)+"\"";
  s += ",\"sd\":"+String(sdReady?"true":"false");
  s += ",\"error\":\""+jsonEscape(lastError)+"\"}";
  return s;
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Energy Monitor</title><style>
body{font:16px Arial;background:#101827;color:#edf2f7;margin:0;padding:18px}
h1{font-size:22px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.card{background:#1d2b40;padding:18px;border-radius:14px}.v{font-size:28px;font-weight:bold;margin-top:8px}
small{color:#aab8ca}a,button{color:white;background:#2563eb;border:0;padding:10px 14px;border-radius:8px;text-decoration:none}
input{width:95%;padding:9px;margin:5px 0 12px;background:#0d1726;color:white;border:1px solid #52627a;border-radius:6px}
label{display:block;color:#b9c6d6}form{max-width:650px}.muted{color:#9fb0c5}
</style><body><h1>⚡ ESP32 Energy Monitor</h1><p id="state" class="muted">Verbinde…</p>
<div class="grid">
<div class="card"><small>Netzbezug aktuell</small><div class="v" id="iw">-- W</div></div>
<div class="card"><small>Einspeisung aktuell</small><div class="v" id="ew">-- W</div></div>
<div class="card"><small>Gesamtverbrauch</small><div class="v" id="ik">-- kWh</div></div>
<div class="card"><small>Gesamteinspeisung</small><div class="v" id="ek">-- kWh</div></div>
</div><p id="updated" class="muted"></p><p><a href="/config">Einstellungen</a> <a href="/api/status">JSON API</a></p>
<script>
async function upd(){try{let r=await fetch('/api/status');let d=await r.json();
document.getElementById('iw').textContent=(d.import_w??0).toFixed(0)+' W';
document.getElementById('ew').textContent=(d.export_w??0).toFixed(0)+' W';
document.getElementById('ik').textContent=(d.import_kwh??0).toFixed(3)+' kWh';
document.getElementById('ek').textContent=(d.export_kwh??0).toFixed(3)+' kWh';
document.getElementById('state').textContent=(d.online?'Lesekopf verbunden':'Lesekopf nicht verbunden')+' · WLAN '+(d.wifi?'verbunden':'getrennt')+' · SD '+(d.sd?'bereit':'Fehler');
document.getElementById('updated').textContent='Letzte Aktualisierung: '+d.updated+(d.error?' · '+d.error:'');
}catch(e){document.getElementById('state').textContent='Verbindung zum ESP32 fehlgeschlagen';}}
upd();setInterval(upd,1000);
</script></body></html>)HTML";

void handleConfig() {
  String h=R"HTML(<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Konfiguration</title><style>body{font:16px Arial;background:#101827;color:#eee;padding:18px}input{display:block;width:95%;padding:9px;margin:5px 0 12px}label{color:#bcd}button{padding:12px;background:#2563eb;color:white;border:0;border-radius:8px}</style><h2>Energy Monitor - Konfiguration</h2><form method="POST" action="/save">)HTML";
  auto field=[&](String label,String name,String value){h+="<label>"+label+"</label><input name='"+name+"' value='"+value+"'>";};
  field("WLAN SSID","ssid",cfg.wifiSsid); field("WLAN Passwort","wpass",cfg.wifiPass);
  field("Lesekopf IP / Hostname","host",cfg.readerHost); field("HTTP Port","port",String(cfg.readerPort));
  field("HTTP-Pfad","path",cfg.readerPath); field("HTTP Benutzer (optional)","user",cfg.readerUser);
  field("HTTP Passwort (optional)","rpass",cfg.readerPass);
  field("JSON-Pfad Gesamtbezug (kWh)","ic",cfg.importCounterPath);
  field("JSON-Pfad Gesamteinspeisung (kWh)","ec",cfg.exportCounterPath);
  field("JSON-Pfad Netzbezug (W)","ip",cfg.importPowerPath);
  field("JSON-Pfad Einspeisung (W)","ep",cfg.exportPowerPath);
  field("JSON-Pfad vorzeichenbehaftete Leistung (W)","sp",cfg.signedPowerPath);
  h+="<label><input style='width:auto;display:inline' type='checkbox' name='signed' value='1' "+String(cfg.useSignedPower?"checked":"")+"> Vorzeichenbehaftete Leistung verwenden</label>";
  h+="<label><input style='width:auto;display:inline' type='checkbox' name='neg' value='1' "+String(cfg.signedExportNegative?"checked":"")+"> Negativer Wert bedeutet Einspeisung</label>";
  field("Abfrageintervall in Sekunden","poll",String(cfg.pollSeconds));
  h+="<button type='submit'>Speichern & Neustart</button></form><p><a href='/'>Zurück</a></p></html>";
  server.send(200,"text/html; charset=utf-8",h);
}

void handleSave() {
  cfg.wifiSsid=server.arg("ssid"); cfg.wifiPass=server.arg("wpass");
  cfg.readerHost=server.arg("host"); cfg.readerPort=server.arg("port").toInt();
  cfg.readerPath=server.arg("path"); cfg.readerUser=server.arg("user"); cfg.readerPass=server.arg("rpass");
  cfg.importCounterPath=server.arg("ic"); cfg.exportCounterPath=server.arg("ec");
  cfg.importPowerPath=server.arg("ip"); cfg.exportPowerPath=server.arg("ep");
  cfg.signedPowerPath=server.arg("sp"); cfg.useSignedPower=server.hasArg("signed");
  cfg.signedExportNegative=server.hasArg("neg");
  cfg.pollSeconds=constrain(server.arg("poll").toInt(),1,60);
  saveSettings();
  server.send(200,"text/html","<meta charset='utf-8'>Gespeichert. Neustart in 2 Sekunden.");
  delay(2000); ESP.restart();
}

void handleDiscover() {
  // mDNS discovery: devices advertising HTTP on the local network.
  int n=MDNS.queryService("http","tcp");
  String out="{\"mdns\":[";
  for(int i=0;i<n;i++) {
    if(i) out+=",";
    out+="{\"host\":\""+String(MDNS.hostname(i))+"\",\"ip\":\""+"\",\"port\":"+String(MDNS.port(i))+"}";
  } 
  out+="],\"note\":\"Zusätzlich kann die IP manuell konfiguriert werden. Nicht alle Leseköpfe senden mDNS.\"}";
  server.send(200,"application/json",out);
}

void setupWeb() {
  server.on("/",HTTP_GET,[]{server.send_P(200,"text/html; charset=utf-8",INDEX_HTML);});
  server.on("/api/status",HTTP_GET,[]{server.send(200,"application/json",statusJson());});
  server.on("/config",HTTP_GET,handleConfig);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/api/discover",HTTP_GET,handleDiscover);
  server.on("/api/test",HTTP_GET,[]{bool ok=fetchReader();server.send(ok?200:502,"application/json",statusJson());});
  server.on("/api/history",HTTP_GET,[]{
    String p=server.arg("date"); if(p.length()!=10){server.send(400,"text/plain","date=YYYY-MM-DD erforderlich");return;}
    String path="/"+p+".csv"; if(!sdReady || !SD.exists(path)){server.send(404,"text/plain","Keine Datei");return;}
    File f=SD.open(path,FILE_READ); server.streamFile(f,"text/csv"); f.close();
  });
  server.onNotFound([]{server.send(404,"text/plain","Nicht gefunden");});
  server.begin();
}

void drawLive() {
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE,TFT_BLACK);
  tft.setTextSize(2); tft.setCursor(8,8); tft.println("ENERGY MONITOR");
  tft.setTextSize(1); tft.setTextColor(readerOnline?TFT_GREEN:TFT_RED,TFT_BLACK);
  tft.setCursor(8,34); tft.println(readerOnline?"Lesekopf verbunden":"Lesekopf offline");
  tft.setTextColor(TFT_WHITE,TFT_BLACK); tft.setTextSize(2);
  tft.setCursor(8,58); tft.printf("Bezug: %.0f W",isnan(liveData.importW)?0:liveData.importW);
  tft.setCursor(8,91); tft.printf("Einspeisung: %.0f W",isnan(liveData.exportW)?0:liveData.exportW);
  tft.setTextSize(1);
  tft.setCursor(8,135); tft.printf("Bezug gesamt: %.3f kWh",isnan(liveData.importKWh)?0:liveData.importKWh);
  tft.setCursor(8,155); tft.printf("Einspeisung: %.3f kWh",isnan(liveData.exportKWh)?0:liveData.exportKWh);
  tft.setCursor(8,190); tft.printf("WLAN: %s",WiFi.status()==WL_CONNECTED?"OK":"Offline");
  tft.setCursor(8,205); tft.printf("SD: %s",sdReady?"OK":"Fehler");
  tft.fillRoundRect(8,260,105,48,8,TFT_BLUE); tft.fillRoundRect(127,260,105,48,8,TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE); tft.setCursor(25,278); tft.print("Einstellungen");
  tft.setCursor(160,278); tft.print("Web-IP");
}

void handleTouch() {
  if(millis()-lastTouchMs<350 || !touch.touched()) return;
  TS_Point p=touch.getPoint(); lastTouchMs=millis();
  // Calibration values are typical only; adjust if touch coordinates are offset.
  // Portrait layout: 240 x 320. Raw touch axes may need calibration per panel.
  int x=map(p.x,TOUCH_X_MIN,TOUCH_X_MAX,0,240);
  int y=map(p.y,TOUCH_Y_MIN,TOUCH_Y_MAX,0,320);
  if(x<0||x>240||y<0||y>320) return;

  if(screenPage==0 && y>=250) {
    if(x<120) screenPage=1; // Settings page
    else screenPage=0;      // Web/IP page shortcut stays on live screen
  } else if(screenPage==1 && y>=250) {
    screenPage=0;
  }

  if(screenPage==1) {
    tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.setTextSize(2);
    tft.setCursor(8,10); tft.println("Einstellungen");
    tft.setTextSize(1); tft.setCursor(8,50); tft.println("Konfiguration im Browser:");
    tft.setCursor(8,75); tft.println(WiFi.localIP().toString());
    tft.setCursor(8,95); tft.println("http://energy-monitor.local");
    tft.fillRoundRect(8,260,220,48,8,TFT_BLUE); tft.setCursor(75,278); tft.print("Zurueck");
  } else drawLive();
}

void setup() {
  Serial.begin(115200);
  loadSettings();

  tft.init(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setCursor(10,20);
  tft.println("Energy Monitor");
  tft.setTextSize(1); tft.println("Starte...");

  touchSPI.begin(TOUCH_CLK,TOUCH_MISO,TOUCH_MOSI,TOUCH_CS);
  touch.begin(touchSPI); touch.setRotation(0);

  sdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdSPI,20000000);
  if(!sdReady) Serial.println("SD-Initialisierung fehlgeschlagen");

  connectWiFi();
  setupWeb();
  if(WiFi.status()==WL_CONNECTED) fetchReader();
  drawLive();
}

void loop() {
  server.handleClient();
  if(WiFi.status()!=WL_CONNECTED && cfg.wifiSsid.length()) {
    static uint32_t lastReconnect=0;
    if(millis()-lastReconnect>15000){lastReconnect=millis();WiFi.reconnect();}
  }

  if(millis()-lastPollMs >= (uint32_t)cfg.pollSeconds*1000) {
    lastPollMs=millis();
    fetchReader();
    if(liveData.valid && isfinite(liveData.importW) && isfinite(liveData.exportW)) {
      importPowerSum+=liveData.importW; exportPowerSum+=liveData.exportW; sampleCount++;
      importPowerMax=max(importPowerMax,liveData.importW); exportPowerMax=max(exportPowerMax,liveData.exportW);
    }
  }
  if(millis()-lastLogMs>=60000) { lastLogMs=millis(); logMinute(); }
  handleTouch();
  if(millis()-lastDrawMs>2000) { lastDrawMs=millis(); if(screenPage==0) drawLive(); }
  delay(5);
}

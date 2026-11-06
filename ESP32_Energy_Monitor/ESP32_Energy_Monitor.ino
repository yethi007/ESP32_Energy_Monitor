/*
  Energy Manager V2.4.2 - ESP32-2432S028 (CYD)
  Features:
  - HTTP/JSON meter-head polling (1 s default)
  - Configurable JSON paths for import/export counters and power
  - Manual HTTP/JSON configuration; optional solar HTTP/JSON source
  - Local web UI and JSON API
  - SD CSV logging once per minute, configurable retention
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
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include "cyd_config.h"

// =============================================================
// Hardware, Datenmodelle und globale Zustandsvariablen
// Alle globalen Deklarationen stehen vor ihrer ersten Verwendung.
// =============================================================
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
SPIClass sdSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
WebServer server(80);
Preferences prefs;

struct Settings {
  String wifiSsid, wifiPass;
  String readerHost = "192.168.178.29";
  uint16_t readerPort = 80;
  String readerPath = "/cm?cmnd=status%208";
  String readerUser = "";
  String readerPass = "";
  String importCounterPath = "StatusSNS.SML.Total_in";
  String exportCounterPath = "StatusSNS.SML.Total_out";
  String importPowerPath = "";
  String exportPowerPath = "";
  String signedPowerPath = "StatusSNS.SML.Power_curr";
  bool useSignedPower = false;
  bool signedExportNegative = true;
  uint16_t pollSeconds = 1;
  String solarHost = "", solarPath = "/", solarUser = "", solarPass = "";
  String solarPowerPath = "", solarTotalPath = "";
  uint16_t solarPort = 80, solarPollSeconds = 5;
  uint16_t retentionDays = 1825;
  bool darkMode = true;
};
Settings cfg;

struct MeterData {
  double importKWh = NAN;
  double exportKWh = NAN;
  double importW = NAN;
  double exportW = NAN;
  time_t updated = 0;
  bool valid = false;
  double rawPowerW = NAN, solarPowerW = NAN, solarTotalKWh = NAN;
  bool solarValid = false;
};
MeterData liveData;

// Zeitsteuerung und Messwert-Aggregation
uint32_t lastPollMs = 0, lastSolarPollMs = 0, lastLogMs = 0, lastDrawMs = 0;
double importPowerSum = 0, exportPowerSum = 0, rawPowerSum = 0, solarPowerSum = 0;
uint32_t sampleCount = 0, rawCount = 0, solarCount = 0;
double importPowerMax = 0, exportPowerMax = 0;

// WLAN, SD und Datenquellen
bool sdReady = false, readerOnline = false, wifiWasConnected = false;
uint32_t lastWiFiReconnect = 0, lastLogPruneMs = 0;
time_t lastReaderSuccess = 0, lastSolarSuccess = 0;
String readerError = "", solarError = "", lastError = "Noch keine Abfrage";

// Fehlerprotokoll: wiederkehrende Meldungen werden begrenzt.
String previousEventKey = "", lastReaderErrorKey = "", lastSolarErrorKey = "";
uint32_t previousEventMs = 0, lastReaderErrorMs = 0, lastSolarErrorMs = 0;

// Tageswerte und Display
int screenPage = 0, dayKey = -1;
double dayImportStart = NAN, daySolarStart = NAN;
uint32_t lastTouchMs = 0;
uint8_t sdUsagePercent = 0;
uint32_t lastSdUsageCheckMs = 0;
bool sdCapacityKnown = false;

// 24h-RAM-Historie: max. 1440 Minutenwerte (ca. 17 KB).
// SD wird nur beim Start schrittweise eingelesen; neue Werte kommen
// nach erfolgreichem CSV-Schreiben auch in diesen Puffer.
constexpr int GRAPH_N = 1440;
float graphNet[GRAPH_N];
float graphSolar[GRAPH_N];
uint32_t graphTime[GRAPH_N];
int graphCount = 0;
uint32_t graphLoadedMs = 0;
bool graphHistoryReady = false;
File historyFile;
int historyDay = 1;
uint32_t historyLines = 0;

// Neustarts erfolgen erst im loop(), nie mitten in einer HTTP-Antwort.
bool restartPending = false;
uint32_t restartRequestedMs = 0;
uint32_t lastSuccessfulAnyMs = 0;
uint32_t bootMs = 0;
uint32_t touchHoldStartMs = 0;
bool touchHoldHandled = false;
String restartReason = "";
constexpr uint32_t DATA_STALE_RESTART_MS = 300000UL;
constexpr uint32_t RESTART_COOLDOWN_SECONDS = 1800UL;


uint16_t bgColor() { return cfg.darkMode ? TFT_BLACK : TFT_WHITE; }
uint16_t fgColor() { return cfg.darkMode ? TFT_WHITE : TFT_BLACK; }

// Funktionsprototypen fuer fruehe Aufrufe (Arduino IDE / C++).
String timestampString(time_t t);
void addHistoryPoint(uint32_t ts, float net, float solar);
void updateSdUsage();
void requestRestart(const String &reason);

String jsonEscape(const String &s) {
  String o; for (size_t i=0;i<s.length();i++) {
    char c=s[i]; if(c=='"' || c=='\\') o+='\\';
    if(c=='\n') o+="\\n"; else if(c!='\r') o+=c;
  } return o;
}

// Logs are independent of measurement CSVs. Only valid NTP timestamps are written.
String logPath(time_t t) {
  struct tm tmv; localtime_r(&t,&tmv); char b[32];
  strftime(b,sizeof(b),"/logs/%Y-%m-%d.log",&tmv); return String(b);
}
void recordEvent(const String &level, const String &source, const String &message) {
  String msg=message; msg.replace("\r"," ");msg.replace("\n"," ");
  String key=level+"|"+source+"|"+msg;
  if(key==previousEventKey && millis()-previousEventMs<600000UL) return;
  previousEventKey=key;previousEventMs=millis();
  Serial.println("["+level+"] "+source+": "+msg);
  time_t now=time(nullptr);
  if(!sdReady || now<1700000000) return;
  if(!SD.exists("/logs") && !SD.mkdir("/logs")) return;
  File f=SD.open(logPath(now),FILE_APPEND);
  if(!f) return;
  if(f.size()>262144UL){f.close();return;}
  f.print(timestampString(now));f.print(" | ");f.print(level);
  f.print(" | ");f.print(source);f.print(" | ");f.println(msg);
  f.close();
}
void reportSourceError(const String &source, const String &message) {
  if(source=="Lesekopf") {
    readerOnline=false;readerError=message;lastError=message;
    if(message!=lastReaderErrorKey || millis()-lastReaderErrorMs>=600000UL) {
      lastReaderErrorKey=message;lastReaderErrorMs=millis();recordEvent("ERROR",source,message);
    }
  } else {
    liveData.solarValid=false;solarError=message;
    if(message!=lastSolarErrorKey || millis()-lastSolarErrorMs>=600000UL) {
      lastSolarErrorKey=message;lastSolarErrorMs=millis();recordEvent("ERROR",source,message);
    }
  }
}
void pruneErrorLogs() {
  if(!sdReady || time(nullptr)<1700000000) return;
  File dir=SD.open("/logs");if(!dir || !dir.isDirectory())return;
  // Keep today plus the preceding 29 local calendar dates.
  time_t now=time(nullptr);struct tm today;localtime_r(&now,&today);
  today.tm_hour=12;today.tm_min=0;today.tm_sec=0;
  time_t cutoff=mktime(&today)-29L*86400L;
  File f=dir.openNextFile();
  while(f){
    String name=String(f.name());bool isDir=f.isDirectory();f.close();
    int slash=name.lastIndexOf('/');String basename=name.substring(slash+1);
    int y=0,m=0,d=0;
    if(!isDir && basename.length()==14 && sscanf(basename.c_str(),"%4d-%2d-%2d.log",&y,&m,&d)==3) {
      struct tm tmv={0};tmv.tm_year=y-1900;tmv.tm_mon=m-1;tmv.tm_mday=d;
      tmv.tm_hour=12;tmv.tm_isdst=-1;
      if(y>=2020 && m>=1 && m<=12 && d>=1 && d<=31 && mktime(&tmv)<cutoff)
        SD.remove("/logs/"+basename);
    }
    f=dir.openNextFile();
  }dir.close();
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
  cfg.solarHost=prefs.getString("shost", ""); cfg.solarPort=prefs.getUShort("sport",80);
  cfg.solarPath=prefs.getString("spath", "/"); cfg.solarUser=prefs.getString("suser", "");
  cfg.solarPass=prefs.getString("spass", ""); cfg.solarPowerPath=prefs.getString("spow", "");
  cfg.solarTotalPath=prefs.getString("stot", ""); cfg.solarPollSeconds=prefs.getUShort("spoll",5);
  cfg.retentionDays=prefs.getUShort("retain",1825);
  cfg.darkMode=prefs.getBool("dark",true);
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
  prefs.putString("shost",cfg.solarHost); prefs.putUShort("sport",cfg.solarPort);
  prefs.putString("spath",cfg.solarPath); prefs.putString("suser",cfg.solarUser);
  prefs.putString("spass",cfg.solarPass); prefs.putString("spow",cfg.solarPowerPath);
  prefs.putString("stot",cfg.solarTotalPath); prefs.putUShort("spoll",cfg.solarPollSeconds);
  prefs.putUShort("retain",cfg.retentionDays);
  prefs.putBool("dark",cfg.darkMode);
  prefs.end();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  if (cfg.wifiSsid.length()) {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-start < 10000) delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("EnergyMonitor-Setup", "energy1234");
  }
  wifiWasConnected=(WiFi.status()==WL_CONNECTED);
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.google.com");
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
  if (WiFi.status() != WL_CONNECTED) { reportSourceError("Lesekopf","WLAN nicht verbunden"); return false; }
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(650); http.setTimeout(950);
  String url=makeReaderUrl();
  if (!http.begin(client, url)) { reportSourceError("Lesekopf","HTTP konnte nicht gestartet werden"); return false; }
  if (cfg.readerUser.length()) http.setAuthorization(cfg.readerUser.c_str(), cfg.readerPass.c_str());
  int code=http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();reportSourceError("Lesekopf","HTTP-Fehler: "+String(code)); return false;
  }
  String payload=http.getString(); http.end();
  JsonDocument doc;
  DeserializationError err=deserializeJson(doc, payload);
  if (err) { reportSourceError("Lesekopf","JSON-Fehler: "+String(err.c_str())); return false; }

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
  if (getPath(doc,cfg.signedPowerPath,v)) liveData.rawPowerW=v;
  else if (gotIP) liveData.rawPowerW=liveData.importW;
  liveData.updated=time(nullptr);
  liveData.valid=gotI || gotE || gotIP || gotEP;
  readerOnline=liveData.valid;
  if(!liveData.valid) {reportSourceError("Lesekopf","JSON ohne konfigurierte Messfelder");return false;}
  if(readerError.length()) recordEvent("INFO","Lesekopf","Verbindung wiederhergestellt");
  readerError="";lastError="";lastReaderSuccess=liveData.updated;
  return true;
}

bool fetchSolar() {
  if (!cfg.solarHost.length()) return false;
  if (WiFi.status()!=WL_CONNECTED) {reportSourceError("Solar","WLAN nicht verbunden");return false;}
  String host=cfg.solarHost;
  if(host.startsWith("http://")) host.remove(0,7);
  String url="http://"+host;
  if(cfg.solarPort!=80) url+=":"+String(cfg.solarPort);
  if(!cfg.solarPath.startsWith("/")) url+="/";
  url+=cfg.solarPath;
  WiFiClient client; HTTPClient http; http.setConnectTimeout(650);http.setTimeout(950);
  if(!http.begin(client,url)) {reportSourceError("Solar","HTTP konnte nicht gestartet werden");return false;}
  if(cfg.solarUser.length()) http.setAuthorization(cfg.solarUser.c_str(),cfg.solarPass.c_str());
  int code=http.GET(); if(code!=HTTP_CODE_OK) {http.end();reportSourceError("Solar","HTTP-Fehler: "+String(code));return false;}
  String body=http.getString(); http.end(); JsonDocument doc;
  if(deserializeJson(doc,body)) {reportSourceError("Solar","JSON konnte nicht gelesen werden");return false;}
  double v; bool a=getPath(doc,cfg.solarPowerPath,v);
  if(a) liveData.solarPowerW=v;
  bool b=getPath(doc,cfg.solarTotalPath,v);
  if(b) liveData.solarTotalKWh=v;
  liveData.solarValid=a||b;
  if(!liveData.solarValid) {reportSourceError("Solar","JSON ohne konfigurierte Messfelder");return false;}
  if(solarError.length())recordEvent("INFO","Solar","Verbindung wiederhergestellt");
  solarError="";lastSolarSuccess=time(nullptr);return true;
}

String jsonNum(double v, int digits=2) {return isfinite(v)?String(v,digits):"null";}

String timestampString(time_t t) {
  if (t < 100000) return "Zeit-nicht-synchron";
  struct tm tmv; localtime_r(&t,&tmv); char b[24];
  strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&tmv); return String(b);
}

void pruneOldFiles() {
  if (!sdReady || time(nullptr)<100000) return;
  time_t cutoff=time(nullptr)-(time_t)cfg.retentionDays*86400L;
  File dir=SD.open("/"); if(!dir) return;
  File f=dir.openNextFile();
  while(f) {
    String name=String(f.name());
    bool isCsv=name.endsWith(".csv") && (name.startsWith("/20") || name.startsWith("20") || name.startsWith("/v2_20") || name.startsWith("v2_20"));
    f.close();
    if(isCsv && name.length()>=11) {
      // Only remove dated CSV files older than the configured retention period.
      struct tm tmv={0}; int y=0,m=0,d=0;
      const char *fname=name.c_str();
      if(fname[0]=='/') fname++;
      if(strncmp(fname,"v2_",3)==0) fname+=3;
      if(strlen(fname)==14 && sscanf(fname,"%d-%d-%d.csv",&y,&m,&d)==3 &&
         y>=2020 && y<=2100 && m>=1 && m<=12 && d>=1 && d<=31) {
        tmv.tm_year=y-1900; tmv.tm_mon=m-1; tmv.tm_mday=d;
        time_t ft=mktime(&tmv); if(ft<cutoff) SD.remove(name);
      }
    }
    f=dir.openNextFile();
  }
  dir.close();
}

// One CSV per local calendar day; ISO-like timestamp with seconds.
String dailyPath(time_t t) {
  struct tm tmv;localtime_r(&t,&tmv);
  char path[24];strftime(path,sizeof(path),"/%Y-%m-%d.csv",&tmv);
  return String(path);
}
int currentDayKey(time_t t) {
  struct tm tmv;localtime_r(&t,&tmv);
  return (tmv.tm_year+1900)*1000+tmv.tm_yday;
}
// Read the first valid counters of the day; survives restarts.
void loadDayBaseline() {
  time_t now=time(nullptr);
  if(now<100000 || !sdReady) return;
  int key=currentDayKey(now);
  if(key==dayKey) return;
  dayKey=key;dayImportStart=NAN;daySolarStart=NAN;
  File f=SD.open(dailyPath(now),FILE_READ);
  if(!f) return;
  String header=f.readStringUntil('\n');header.trim();
  if(header!="timestamp,import_kwh,export_kwh,net_power_w,solar_power_w,solar_total_kwh") {f.close();return;}
  while(f.available() && (!isfinite(dayImportStart)||!isfinite(daySolarStart))) {
    String line=f.readStringUntil('\n');
    int c1=line.indexOf(','),c2=line.indexOf(',',c1+1),c3=line.indexOf(',',c2+1);
    if(c1<0||c2<0||c3<0) continue;
    String imp=line.substring(c1+1,c2);
    String solar=line.substring(c3+1); // remaining columns: find solar_total (column 6)
    int start=c3+1;
    for(int col=3;col<5;col++){int k=line.indexOf(',',start);if(k<0){start=-1;break;}start=k+1;}
    if(start>=0){int k=line.indexOf(',',start);solar=(k<0)?line.substring(start):line.substring(start,k);}
    if(!isfinite(dayImportStart) && imp!="null" && imp.length()) dayImportStart=imp.toDouble();
    if(!isfinite(daySolarStart) && solar!="null" && solar.length()) daySolarStart=solar.toDouble();
  }
  f.close();
}
double todayImport() {return isfinite(dayImportStart)&&isfinite(liveData.importKWh)?max(0.0,liveData.importKWh-dayImportStart):NAN;}
double todaySolar() {return isfinite(daySolarStart)&&isfinite(liveData.solarTotalKWh)?max(0.0,liveData.solarTotalKWh-daySolarStart):NAN;}
void logMinute() {
  if(!sdReady || time(nullptr)<100000) return;
  time_t now=time(nullptr);
  loadDayBaseline();
  String path=dailyPath(now);
  bool exists=SD.exists(path);
  if(exists){
    File old=SD.open(path,FILE_READ);
    String header=old?old.readStringUntil('\n'):String("");
    if(old)old.close();header.trim();
    if(header!="timestamp,import_kwh,export_kwh,net_power_w,solar_power_w,solar_total_kwh"){
      // Preserve the old format under a non-CSV backup name.
      String backup=path+".legacy.bak";
      if(SD.exists(backup)){
        lastError="Alte Tagesdatei: Backup bereits vorhanden";return;
      }
      if(!SD.rename(path,backup)){lastError="Alte Tagesdatei konnte nicht gesichert werden";recordEvent("ERROR","SD",lastError);return;}
      exists=false;
    }
  }
  File f=SD.open(path,FILE_APPEND);
  if(!f){lastError="SD-Schreibfehler";recordEvent("ERROR","SD","Messdaten konnten nicht geschrieben werden");return;}
  if(!exists) f.println("timestamp,import_kwh,export_kwh,net_power_w,solar_power_w,solar_total_kwh");
  String ts=timestampString(now);ts.replace(' ','-');ts.replace(':','-');
  double net=rawCount?rawPowerSum/rawCount:NAN;
  double solar=solarCount?solarPowerSum/solarCount:NAN;
  f.print(ts);f.print(',');f.print(jsonNum(liveData.importKWh,5));f.print(',');
  f.print(jsonNum(liveData.exportKWh,5));f.print(',');f.print(jsonNum(net,2));f.print(',');
  f.print(jsonNum(solar,2));f.print(',');f.println(jsonNum(liveData.solarTotalKWh,5));
  f.close();
  if(graphHistoryReady) addHistoryPoint((uint32_t)now,(float)net,(float)solar);
  if(!isfinite(dayImportStart)&&isfinite(liveData.importKWh))dayImportStart=liveData.importKWh;
  if(!isfinite(daySolarStart)&&isfinite(liveData.solarTotalKWh))daySolarStart=liveData.solarTotalKWh;
  importPowerSum=exportPowerSum=rawPowerSum=solarPowerSum=0;
  sampleCount=rawCount=solarCount=0;importPowerMax=exportPowerMax=0;
  static int pruned=-1;
  int key=currentDayKey(now);
  if(key!=pruned){pruned=key;pruneOldFiles();}
}

String statusJson() {
  String s="{";
  s.reserve(850); // Reduziert Heap-Reallokationen beim JSON-Aufbau.
  s += "\"online\":" + String(readerOnline?"true":"false");
  s += ",\"wifi\":" + String(WiFi.status()==WL_CONNECTED?"true":"false");
  s += ",\"ip\":\""+WiFi.localIP().toString()+"\"";
  s += ",\"import_kwh\":"+jsonNum(liveData.importKWh,3);
  s += ",\"export_kwh\":"+jsonNum(liveData.exportKWh,3);
  s += ",\"import_w\":"+jsonNum(liveData.importW,1);
  s += ",\"export_w\":"+jsonNum(liveData.exportW,1);
  s += ",\"net_w\":"+jsonNum(liveData.rawPowerW,1);
  s += ",\"solar_w\":"+jsonNum(liveData.solarPowerW,1);
  s += ",\"solar_kwh\":"+jsonNum(liveData.solarTotalKWh,3);
  s += ",\"today_import_kwh\":"+jsonNum(todayImport(),3);
  s += ",\"today_solar_kwh\":"+jsonNum(todaySolar(),3);
  s += ",\"dark_mode\":"+String(cfg.darkMode?"true":"false");
  s += ",\"updated\":\""+timestampString(liveData.updated)+"\"";
  s += ",\"sd\":"+String(sdReady?"true":"false");
  s += ",\"sd_used_percent\":"+(sdCapacityKnown?String(sdUsagePercent):String("null"));
  s += ",\"rssi\":"+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0);
  s += ",\"reader_last_ok\":\""+timestampString(lastReaderSuccess)+"\"";
  s += ",\"solar_last_ok\":\""+timestampString(lastSolarSuccess)+"\"";
  s += ",\"reader_error\":\""+jsonEscape(readerError)+"\"";
  s += ",\"solar_error\":\""+jsonEscape(solarError)+"\"";
  // Speicherdiagnose: niedriger Minimum-Heap weist auf Engpaesse hin.
  s += ",\"heap_free\":"+String(ESP.getFreeHeap());
  s += ",\"heap_min\":"+String(ESP.getMinFreeHeap());
  s += ",\"heap_max_block\":"+String(ESP.getMaxAllocHeap());
  s += ",\"uptime_s\":"+String(millis()/1000UL);
  s += ",\"restart_pending\":"+String(restartPending?"true":"false");
  s += ",\"history_points\":"+String(graphCount);
  s += ",\"history_ready\":"+String(graphHistoryReady?"true":"false");
  s += ",\"error\":\""+jsonEscape(lastError)+"\"}";
  return s;
}

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Energy Manager</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}body{font:16px system-ui,Arial;background:#101827;color:#f8fafc;margin:0;padding:16px}
main{max-width:920px;margin:auto}header{display:flex;align-items:center;gap:12px;margin-bottom:16px}header .logo{background:#1e3a5f;color:#facc15;font-size:28px;border-radius:12px;padding:8px 13px}
h1{font-size:24px;margin:0}small,.muted{color:#9fb0c5}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.card{background:#1d2b40;border-radius:14px;padding:16px;min-width:0}.label{font-size:14px;font-weight:700;color:#93c5fd}.pv .label{color:#facc15}
.value{font-size:clamp(25px,5vw,40px);font-weight:750;margin:8px 0}.detail{font-size:13px;color:#cbd5e1;line-height:1.8}
nav{display:flex;flex-wrap:wrap;gap:9px;margin:16px 0}a,button{background:#2563eb;color:white;padding:10px 13px;border:0;border-radius:9px;text-decoration:none;cursor:pointer;font:inherit}
footer{font-size:13px;color:#b9c7d9;margin-top:16px}.ok{color:#4ade80}.bad{color:#f87171}.warn{color:#facc15}
@media(max-width:500px){body{padding:10px}.card{padding:12px}.detail{font-size:12px}h1{font-size:21px}}
</style></head><body><main>
<header><div class="logo">⚡</div><div style="flex:1"><h1>Energy Manager</h1><small>Live-Monitor</small></div><div id="icons">WLAN · SD</div></header>
<div class="grid">
<section class="card"><div class="label">NETZ</div><div class="value" id="nw">-- W</div><div class="detail">Abruf: <span id="nr">--</span><br>Heute: <span id="di">--</span> kWh<br>Gesamt: <span id="ik">--</span> kWh</div></section>
<section class="card pv"><div class="label">PV</div><div class="value" id="sw">-- W</div><div class="detail">Abruf: <span id="sr">--</span><br>Heute: <span id="ds">--</span> kWh<br>Gesamt: <span id="sk">--</span> kWh</div></section>
</div>
<nav><a href="/chart">24h-Grafik</a><a href="/config">Einstellungen</a><a href="/status">Status</a><a href="/logs">Fehlerprotokoll</a></nav>
<section class="card"><div id="state">Verbinde…</div><div class="detail" id="extra"></div></section>
<footer id="foot">--</footer>
<script>
const el=id=>document.getElementById(id);let busy=false,controller=null;
const num=(v,n)=>v==null?'--':Number(v).toFixed(n);
function clock(s){return s&&s.length>=8?s.slice(-8):'--'}
async function update(){if(busy||document.hidden)return;busy=true;controller=new AbortController();
const timeout=setTimeout(()=>controller.abort(),2500);
try{const r=await fetch('/api/status',{cache:'no-store',signal:controller.signal});if(!r.ok)throw Error(r.status);
const d=await r.json();el('nw').textContent=num(d.net_w,0)+' W';el('sw').textContent=num(d.solar_w,0)+' W';
el('nr').textContent=clock(d.reader_last_ok);el('sr').textContent=clock(d.solar_last_ok);
el('di').textContent=num(d.today_import_kwh,2);el('ds').textContent=num(d.today_solar_kwh,2);
el('ik').textContent=num(d.import_kwh,2);el('sk').textContent=num(d.solar_kwh,2);
el('icons').innerHTML='<span class="'+(d.wifi?'ok':'bad')+'">● WLAN</span> <span class="'+(d.sd?'ok':'bad')+'">● SD</span>';
el('state').textContent='Netzbezug / Einspeisung: '+num(d.import_w,0)+' / '+num(d.export_w,0)+' W';
el('extra').textContent='Lesekopf: '+(d.online?'OK':'keine Daten')+' · WLAN '+d.rssi+' dBm · SD '+(d.sd_used_percent??'--')+' % · Heap '+Math.round(d.heap_free/1024)+' KB';
el('foot').textContent='IP: '+d.ip+' · Laufzeit '+Math.floor(d.uptime_s/60)+' min';
}catch(e){el('state').textContent='Statusabfrage fehlgeschlagen – erneuter Versuch folgt';}
finally{clearTimeout(timeout);busy=false;controller=null;}}
update();setInterval(update,5000);document.addEventListener('visibilitychange',()=>{if(document.hidden&&controller)controller.abort();else if(!document.hidden)update()});
</script></main></body></html>)HTML";

void handleChartPage() {
  static const char PAGE[] PROGMEM=R"HTML(<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Energy Manager - 24h</title><style>body{font:16px Arial;background:#101827;color:#edf2f7;margin:20px}a{color:#8bc9ff}canvas{max-width:100%;background:#1d2b40;border-radius:9px}</style><h2>Leistung der letzten 24 Stunden</h2><p><a href="/">Zur Live-Anzeige</a></p><canvas id="graph" width="900" height="300" style="width:100%;height:300px"></canvas><p id="hint">Historie wird geladen ...</p><script>
let chartBusy=false;async function drawChart(){if(chartBusy||document.hidden)return;chartBusy=true;const ctrl=new AbortController();const timer=setTimeout(()=>ctrl.abort(),12000);try{
const r=await fetch('/api/chart24',{cache:'no-store',signal:ctrl.signal});if(!r.ok)throw Error(r.status);const d=await r.json();const pts=d.points||[];
document.getElementById('hint').textContent=d.ready?'Netz (blau) · PV (gelb) · max. 288 Punkte':'Startdaten werden einmalig geladen ...';const c=document.getElementById('graph'),ctx=c.getContext('2d');const W=c.width,H=c.height;
ctx.fillStyle='#1d2b40';ctx.fillRect(0,0,W,H);
const vals=pts.flatMap(p=>[p.w,p.solar]).filter(v=>v!==null&&Number.isFinite(v));
let hi=Math.max(100,...vals),lo=Math.min(0,...vals);if(hi===lo)hi=lo+100;
const L=55,R=W-15,T=20,B=H-35;
ctx.strokeStyle='#52627a';ctx.fillStyle='#aab8ca';ctx.font='12px Arial';
for(let i=0;i<=4;i++){let y=T+i*(B-T)/4;ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(R,y);ctx.stroke();ctx.fillText(Math.round(hi-(hi-lo)*i/4)+' W',3,y+4);}
const now=Date.now()/1000,start=now-86400;
function line(key,color){ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();let active=false;
for(const p of pts){const v=p[key];if(v===null||!Number.isFinite(v)){active=false;continue;}
const x=L+Math.max(0,Math.min(1,(p.t-start)/86400))*(R-L),y=B-(v-lo)/(hi-lo)*(B-T);
if(!active){ctx.moveTo(x,y);active=true;}else ctx.lineTo(x,y);}ctx.stroke();}
line('w','#38bdf8');line('solar','#facc15');ctx.fillStyle='#aab8ca';ctx.fillText('-24 h',L,B+20);ctx.fillText('Jetzt',R-35,B+20);
}catch(e){document.getElementById('hint').textContent='Grafik nicht erreichbar: '+e.message;}finally{clearTimeout(timer);chartBusy=false;}}
drawChart();setInterval(drawChart,60000);
</script></html>)HTML";
  server.send_P(200,"text/html; charset=utf-8",PAGE);
}

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
  field("Abfrageintervall Lesekopf (Sekunden)","poll",String(cfg.pollSeconds));
  h+="<h3>Solaranlage (optional)</h3>";
  field("Solar IP / Hostname","shost",cfg.solarHost);
  field("Solar HTTP Port","sport",String(cfg.solarPort));
  field("Solar HTTP-Pfad","spath",cfg.solarPath);
  field("Solar HTTP Benutzer","suser",cfg.solarUser);
  field("Solar HTTP Passwort","spass",cfg.solarPass);
  field("Solar JSON-Pfad aktuelle Leistung (W)","spow",cfg.solarPowerPath);
  field("Solar JSON-Pfad Gesamterzeugung (kWh)","stot",cfg.solarTotalPath);
  field("Solar-Abfrageintervall (Sekunden)","spoll",String(cfg.solarPollSeconds));
  field("Historie aufbewahren (Tage)","retain",String(cfg.retentionDays));
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
  cfg.solarHost=server.arg("shost"); cfg.solarPort=constrain(server.arg("sport").toInt(),1,65535);
  cfg.solarPath=server.arg("spath"); cfg.solarUser=server.arg("suser"); cfg.solarPass=server.arg("spass");
  cfg.solarPowerPath=server.arg("spow"); cfg.solarTotalPath=server.arg("stot");
  cfg.solarPollSeconds=constrain(server.arg("spoll").toInt(),1,3600);
  cfg.retentionDays=constrain(server.arg("retain").toInt(),1,36500);
  saveSettings();
  server.send(200,"text/html","<meta charset='utf-8'>Gespeichert. Neustart in 2 Sekunden.");
  delay(2000); ESP.restart();
}

void addHistoryPoint(uint32_t ts,float net,float solar) {
  if(ts<1700000000UL) return;
  // Ignore duplicate or out-of-order records, e.g. the same minute on restart.
  if(graphCount && ts<=graphTime[graphCount-1]) {
    if(ts==graphTime[graphCount-1]) {
      graphNet[graphCount-1]=net;graphSolar[graphCount-1]=solar;
    }
    return;
  }
  if(graphCount==GRAPH_N) {
    memmove(graphTime,graphTime+1,(GRAPH_N-1)*sizeof(graphTime[0]));
    memmove(graphNet,graphNet+1,(GRAPH_N-1)*sizeof(graphNet[0]));
    memmove(graphSolar,graphSolar+1,(GRAPH_N-1)*sizeof(graphSolar[0]));
    graphCount--;
  }
  graphTime[graphCount]=ts;graphNet[graphCount]=net;graphSolar[graphCount]=solar;
  graphCount++;graphLoadedMs=millis();
  uint32_t cutoff=(uint32_t)time(nullptr)-86400UL;
  // At most one removal per new point; the daily cadence keeps the ring bounded.
  while(graphCount && graphTime[0]<cutoff) {
    graphCount--;
    if(graphCount) {
      memmove(graphTime,graphTime+1,graphCount*sizeof(graphTime[0]));
      memmove(graphNet,graphNet+1,graphCount*sizeof(graphNet[0]));
      memmove(graphSolar,graphSolar+1,graphCount*sizeof(graphSolar[0]));
    }
  }
}

void parseHistoryLine(const String &line) {
  int a=line.indexOf(','),b=line.indexOf(',',a+1),c=line.indexOf(',',b+1);
  int d=line.indexOf(',',c+1),e=line.indexOf(',',d+1);
  if(a<0||b<0||c<0||d<0||e<0)return;
  int yy,mo,dd,hh,mm,ss;
  if(sscanf(line.c_str(),"%d-%d-%d-%d-%d-%d",&yy,&mo,&dd,&hh,&mm,&ss)!=6)return;
  struct tm t={0};t.tm_year=yy-1900;t.tm_mon=mo-1;t.tm_mday=dd;
  t.tm_hour=hh;t.tm_min=mm;t.tm_sec=ss;t.tm_isdst=-1;
  time_t ts=mktime(&t),now=time(nullptr);
  if(ts<now-86400L||ts>now)return;
  String nw=line.substring(c+1,d),sw=line.substring(d+1,e);
  addHistoryPoint((uint32_t)ts,nw=="null"?NAN:nw.toFloat(),sw=="null"?NAN:sw.toFloat());
}

// Incremental boot-time loading: max 12 lines per loop, so web/display keep running.
void serviceHistoryStartup() {
  if(graphHistoryReady || !sdReady || time(nullptr)<1700000000) return;
  if(!historyFile) {
    if(historyDay<0) {graphHistoryReady=true;graphLoadedMs=millis();return;}
    time_t now=time(nullptr);
    String path=dailyPath(now-(time_t)historyDay*86400L);
    historyFile=SD.open(path,FILE_READ);
    if(!historyFile) {historyDay--;return;}
    String header=historyFile.readStringUntil('\n');header.trim();
    if(header!="timestamp,import_kwh,export_kwh,net_power_w,solar_power_w,solar_total_kwh") {
      historyFile.close();historyDay--;return;
    }
  }
  for(int i=0;i<12 && historyFile.available();i++) {
    parseHistoryLine(historyFile.readStringUntil('\n'));
    historyLines++;
  }
  if(!historyFile.available()) {historyFile.close();historyDay--;}
}

void drawGraph() {
  tft.fillScreen(bgColor());tft.setTextColor(fgColor(),bgColor());
  tft.setTextSize(2);tft.setCursor(7,5);tft.print("Leistung 24h");
  tft.setTextSize(1);tft.setCursor(7,27);tft.setTextColor(TFT_CYAN);tft.print("Netz");
  tft.setCursor(65,27);tft.setTextColor(TFT_YELLOW);tft.print("Solar");
  int left=34,top=45,right=314,bottom=207;
  tft.drawRect(left,top,right-left,bottom-top,TFT_DARKGREY);
  float hi=100,lo=0;
  for(int i=0;i<graphCount;i++){
    if(isfinite(graphNet[i])) {hi=max(hi,graphNet[i]);lo=min(lo,graphNet[i]);}
    if(isfinite(graphSolar[i])) hi=max(hi,graphSolar[i]);
  }
  if(hi<=lo) hi=lo+100;
  tft.setTextColor(TFT_LIGHTGREY);tft.setCursor(0,top);tft.printf("%.0f",hi);
  tft.setCursor(0,bottom-7);tft.printf("%.0f",lo);
  time_t now=time(nullptr);time_t start=now-86400L;
  for(int i=1;i<graphCount;i++){
    int x1=left+((int64_t)(graphTime[i-1]-start)*(right-left))/86400;
    int x2=left+((int64_t)(graphTime[i]-start)*(right-left))/86400;
    x1=constrain(x1,left,right);x2=constrain(x2,left,right);
    if(isfinite(graphNet[i-1])&&isfinite(graphNet[i])) {
      int y1=bottom-(graphNet[i-1]-lo)*(bottom-top)/(hi-lo);
      int y2=bottom-(graphNet[i]-lo)*(bottom-top)/(hi-lo);
      tft.drawLine(x1,constrain(y1,top,bottom),x2,constrain(y2,top,bottom),TFT_CYAN);
    }
    if(isfinite(graphSolar[i-1])&&isfinite(graphSolar[i])) {
      int y1=bottom-(graphSolar[i-1]-lo)*(bottom-top)/(hi-lo);
      int y2=bottom-(graphSolar[i]-lo)*(bottom-top)/(hi-lo);
      tft.drawLine(x1,constrain(y1,top,bottom),x2,constrain(y2,top,bottom),TFT_YELLOW);
    }
  }
  tft.setTextColor(fgColor());tft.setCursor(35,211);tft.print("-24h");
  tft.setCursor(284,211);tft.print("Jetzt");
  tft.setCursor(100,229);tft.print("Tippen: Dark/Light");
}

void handleChart24() {
  // Maximal 288 Datenpunkte. Kleine, feste HTTP-Bloecke statt 288
  // einzelner sendContent()-Aufrufe (deutlich weniger TCP-Overhead).
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json; charset=utf-8","");
  server.sendContent("{\"ready\":"+String(graphHistoryReady?"true":"false")+",\"points\":[");
  const int stride=max(1,(graphCount+287)/288);
  String chunk; chunk.reserve(1100);
  bool first=true;
  for(int i=0;i<graphCount;i+=stride) {
    double nw=0,sw=0;int nc=0,sc=0;
    for(int j=i;j<min(i+stride,graphCount);j++) {
      if(isfinite(graphNet[j])){nw+=graphNet[j];nc++;}
      if(isfinite(graphSolar[j])){sw+=graphSolar[j];sc++;}
    }
    String item=String(first?"":",")+"{\"t\":"+String(graphTime[i])+",\"w\":"+
      jsonNum(nc?nw/nc:NAN,1)+",\"solar\":"+jsonNum(sc?sw/sc:NAN,1)+"}";
    first=false;
    if(chunk.length()+item.length()>900) {
      server.sendContent(chunk);chunk="";
      yield();
    }
    chunk+=item;
  }
  if(chunk.length())server.sendContent(chunk);
  server.sendContent("]}");
  server.sendContent(""); // Terminator fuer Chunked-Encoding.
}

void requestRestart(const String &reason) {
  if(restartPending)return;
  restartReason=reason;
  restartPending=true;
  restartRequestedMs=millis();
  Serial.println("Neustart angefordert: "+reason);
}

void handleRestart() {
  // Nur POST, nicht als GET-Link ausloesbar.
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"text/html; charset=utf-8",
    "<meta charset='utf-8'><title>Neustart</title><p>Neustart angefordert. Bitte etwa 15 Sekunden warten.</p><a href='/'>Zur Startseite</a>");
  requestRestart("Manueller Neustart ueber Web");
}

void handleStatusPage() {
  static const char PAGE[] PROGMEM=R"HTML(<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Systemstatus</title>
<style>body{font:16px system-ui,Arial;background:#101827;color:#f8fafc;margin:20px;max-width:850px}a{color:#93c5fd}pre{background:#1d2b40;padding:15px;border-radius:10px;white-space:pre-wrap;overflow-wrap:anywhere}button{background:#b91c1c;color:white;border:0;padding:12px 16px;border-radius:9px;cursor:pointer}</style>
<h2>Energy Manager · Systemstatus</h2><p><a href="/">Startseite</a> · <a href="/api/status">JSON API</a></p><pre id="data">Lade…</pre>
<button id="restart">System neu starten</button><script>
fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(d=>document.getElementById('data').textContent=JSON.stringify(d,null,2)).catch(()=>document.getElementById('data').textContent='Nicht erreichbar');
document.getElementById('restart').onclick=async()=>{if(!confirm('Energy Manager wirklich neu starten?'))return;const r=await fetch('/api/restart',{method:'POST'});document.body.innerHTML=await r.text()};
</script></html>)HTML";
  server.send_P(200,"text/html; charset=utf-8",PAGE);
}

void handleLogs() {
  String h=R"HTML(<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Fehlerprotokoll</title>
<style>body{font:15px Arial;background:#101827;color:#edf2f7;padding:18px}a{color:#8bc9ff}pre{background:#1d2b40;padding:14px;white-space:pre-wrap;overflow-wrap:anywhere;border-radius:8px}input,button{padding:8px}</style>
<h2>Fehlerprotokoll</h2><p>Aufbewahrung: heute und 29 vorherige Tage. Gleiche Fehler werden höchstens alle 10 Minuten erneut geschrieben.</p>
<p><a href="/">Startseite</a> · <a href="/config">Einstellungen</a></p>
<label>Datum: <input id="day" type="date"></label><button onclick="load()">Anzeigen</button>
<p><a id="download" href="/api/logs">Protokoll herunterladen</a></p><pre id="content">Lade…</pre>
<script>
let d=new Date();let z=n=>String(n).padStart(2,'0');document.getElementById('day').value=d.getFullYear()+'-'+z(d.getMonth()+1)+'-'+z(d.getDate());
async function load(){let date=document.getElementById('day').value;let url='/api/logs?date='+encodeURIComponent(date);
document.getElementById('download').href=url;
try{let r=await fetch(url);document.getElementById('content').textContent=r.ok?await r.text():'Für diesen Tag liegt kein Protokoll vor.';}catch(e){document.getElementById('content').textContent='Fehler beim Laden.';}}
load();</script></html>)HTML";
  server.send(200,"text/html; charset=utf-8",h);
}
bool validLogDate(const String &d) {
  if(d.length()!=10 || d[4]!='-' || d[7]!='-')return false;
  for(int i=0;i<10;i++)if(i!=4 && i!=7 && (d[i]<'0'||d[i]>'9'))return false;
  int y=d.substring(0,4).toInt(),m=d.substring(5,7).toInt(),day=d.substring(8,10).toInt();
  if(y<2020||y>2100||m<1||m>12||day<1||day>31)return false;
  struct tm tmv={0};tmv.tm_year=y-1900;tmv.tm_mon=m-1;tmv.tm_mday=day;tmv.tm_hour=12;tmv.tm_isdst=-1;
  time_t t=mktime(&tmv);time_t now=time(nullptr);
  return now>1700000000 && t<=now+86400L && t>=now-31L*86400L;
}
void handleLogDownload(){
  String date=server.arg("date");
  if(!validLogDate(date)){server.send(400,"text/plain","Datum YYYY-MM-DD erforderlich (letzte 30 Tage)");return;}
  String path="/logs/"+date+".log";
  if(!sdReady || !SD.exists(path)){server.send(404,"text/plain","Kein Fehlerprotokoll vorhanden");return;}
  File f=SD.open(path,FILE_READ);
  if(!f){server.send(500,"text/plain","Protokoll nicht lesbar");return;}
  server.streamFile(f,"text/plain; charset=utf-8");f.close();
}

void setupWeb() {
  server.on("/",HTTP_GET,[]{server.send_P(200,"text/html; charset=utf-8",INDEX_HTML);});
  server.on("/api/status",HTTP_GET,[]{server.send(200,"application/json",statusJson());});
  server.on("/api/chart24",HTTP_GET,handleChart24);
  server.on("/status",HTTP_GET,handleStatusPage);
  server.on("/api/restart",HTTP_POST,handleRestart);
  server.on("/chart",HTTP_GET,handleChartPage);
  server.on("/config",HTTP_GET,handleConfig);
  server.on("/logs",HTTP_GET,handleLogs);
  server.on("/api/logs",HTTP_GET,handleLogDownload);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/api/test",HTTP_GET,[]{lastPollMs=millis()-(uint32_t)cfg.pollSeconds*1000UL;server.send(202,"application/json","{\"scheduled\":true}");});
  server.on("/api/history",HTTP_GET,[]{
    String p=server.arg("date"); if(p.length()!=10){server.send(400,"text/plain","date=YYYY-MM-DD erforderlich");return;}
    String path="/"+p+".csv"; if(!sdReady || !SD.exists(path)){server.send(404,"text/plain","Keine Datei");return;}
    File f=SD.open(path,FILE_READ); server.streamFile(f,"text/csv"); f.close();
  });
  server.onNotFound([]{server.send(404,"text/plain","Nicht gefunden");});
  server.begin();
}

// SD-Belegung wird selten abgefragt; SD.totalBytes() ist nicht billig.
void updateSdUsage() {
  if (!sdReady) { sdCapacityKnown=false; return; }
  if (lastSdUsageCheckMs && millis()-lastSdUsageCheckMs<600000UL) return;
  lastSdUsageCheckMs=millis();
  uint64_t total=SD.totalBytes();
  uint64_t used=SD.usedBytes();
  sdCapacityKnown=total>0;
  if (sdCapacityKnown) sdUsagePercent=(uint8_t)min((uint64_t)100,(used*100ULL)/total);
}
String lastOkClock(time_t ts) {
  if(ts<1700000000) return "--:--:--";
  struct tm tmv;localtime_r(&ts,&tmv);
  char b[12];strftime(b,sizeof(b),"%H:%M:%S",&tmv);
  return String(b);
}
uint16_t freshnessColor(time_t lastOk, uint16_t normalColor) {
  time_t now=time(nullptr);
  if(lastOk<1700000000 || now<lastOk) return TFT_RED;
  if(now-lastOk>60) return TFT_RED;
  if(now-lastOk>30) return TFT_ORANGE;
  return normalColor;
}
// Small native TFT icon: solar disc and energy lightning bolt.
void drawEnergyLogo(int x,int y) {
  uint16_t solar=TFT_YELLOW, bolt=TFT_CYAN;
  tft.drawCircle(x+10,y+10,8,solar);
  tft.fillCircle(x+10,y+10,5,solar);
  for(int i=0;i<8;i++) {
    float a=i*0.785398f;
    tft.drawLine(x+10+(int)(10*cosf(a)),y+10+(int)(10*sinf(a)),
                 x+10+(int)(13*cosf(a)),y+10+(int)(13*sinf(a)),solar);
  }
  tft.fillTriangle(x+21,y+1,x+15,y+13,x+23,y+13,bolt);
  tft.fillTriangle(x+23,y+9,x+18,y+22,x+28,y+9,bolt);
}
void drawLive() {
  updateSdUsage();
  uint16_t fg=fgColor(), bg=bgColor();
  tft.fillScreen(bg);
  drawEnergyLogo(5,3);
  tft.setTextColor(fg,bg);tft.setTextSize(2);
  tft.setCursor(38,6);tft.print("Energy Manager");
  tft.drawFastHLine(5,31,310,TFT_DARKGREY);

  // Current power and the timestamp of the last successful data retrieval.
  tft.setTextSize(1);tft.setTextColor(TFT_CYAN,bg);
  tft.setCursor(7,40);tft.print("NETZ");
  tft.setTextColor(TFT_YELLOW,bg);tft.setCursor(166,40);tft.print("PV");
  tft.setTextSize(2);tft.setTextColor(fg,bg);
  tft.setCursor(7,56);
  if(isfinite(liveData.rawPowerW)) tft.printf("%.0f W",liveData.rawPowerW);
  else tft.print("-- W");
  tft.setCursor(166,56);
  if(isfinite(liveData.solarPowerW)) tft.printf("%.0f W",liveData.solarPowerW);
  else tft.print("-- W");
  tft.setTextSize(1);
  tft.setTextColor(freshnessColor(lastReaderSuccess,fg),bg);
  tft.setCursor(7,78);tft.print("Abruf "+lastOkClock(lastReaderSuccess));
  tft.setTextColor(freshnessColor(lastSolarSuccess,fg),bg);
  tft.setCursor(166,78);tft.print("Abruf "+lastOkClock(lastSolarSuccess));

  // Lifetime counters (small print, always visible).
  tft.setTextColor(fg,bg);
  tft.setCursor(7,94);tft.print("Bezug gesamt:");
  tft.setCursor(7,106);tft.print(jsonNum(liveData.importKWh,2)+" kWh");
  tft.setCursor(166,94);tft.print("PV gesamt:");
  tft.setCursor(166,106);tft.print(jsonNum(liveData.solarTotalKWh,2)+" kWh");

  tft.drawFastHLine(5,123,310,TFT_DARKGREY);
  tft.setTextColor(TFT_LIGHTGREY,bg);
  tft.setCursor(7,131);tft.print("Netzbezug heute");
  tft.setCursor(166,131);tft.print("PV-Erzeugung heute");
  tft.setTextColor(fg,bg);tft.setTextSize(2);
  tft.setCursor(7,146);tft.print(jsonNum(todayImport(),2)+" kWh");
  tft.setCursor(166,146);tft.print(jsonNum(todaySolar(),2)+" kWh");

  tft.drawFastHLine(5,174,310,TFT_DARKGREY);
  bool wlan=WiFi.status()==WL_CONNECTED;
  tft.setTextSize(1);tft.setTextColor(wlan?TFT_GREEN:TFT_RED,bg);
  tft.setCursor(7,182);tft.print(wlan?"WLAN OK":"WLAN AUS");
  tft.setTextColor(fg,bg);tft.setCursor(80,182);
  if(wlan) tft.printf("%d dBm",WiFi.RSSI());else tft.print("-- dBm");
  tft.setTextColor(sdReady?TFT_GREEN:TFT_RED,bg);
  tft.setCursor(183,182);tft.print(sdReady?"SD OK":"SD FEHLER");

  tft.setTextColor(fg,bg);tft.setCursor(7,200);
  tft.print("IP: "+(wlan?WiFi.localIP().toString():String("--")));
  tft.setCursor(210,200);
  if(!sdReady) tft.print("SD: Fehler");
  else if(sdCapacityKnown) tft.printf("SD: %u%%",(unsigned)sdUsagePercent);
  else tft.print("SD: --");

  tft.drawFastHLine(5,216,310,TFT_DARKGREY);
  tft.setTextColor(TFT_CYAN,bg);tft.setCursor(7,226);
  tft.print("Tippen: 24h-Grafik / Dark-Light");
}

void handleTouch() {
  // Auf der Systemseite: 3 Sekunden gedrueckt halten = Neustart.
  bool pressed=touch.touched();
  if(pressed) {
    if(!touchHoldStartMs) {touchHoldStartMs=millis();touchHoldHandled=false;}
    if(screenPage==2 && !touchHoldHandled && millis()-touchHoldStartMs>=3000UL) {
      touchHoldHandled=true;
      requestRestart("Manueller Neustart ueber Touch");
      tft.fillScreen(bgColor());tft.setTextColor(fgColor());tft.setCursor(20,80);
      tft.setTextSize(2);tft.print("Neustart...");
    }
    return;
  }
  if(!touchHoldStartMs)return;
  uint32_t held=millis()-touchHoldStartMs;
  touchHoldStartMs=0;
  if(touchHoldHandled || held<40 || millis()-lastTouchMs<400)return;
  lastTouchMs=millis();
  if(screenPage==0) {screenPage=1;drawGraph();}
  else if(screenPage==1) {
    screenPage=2;
    tft.fillScreen(bgColor());tft.setTextColor(fgColor());tft.setTextSize(2);
    tft.setCursor(15,20);tft.print("System / Anzeige");
    tft.setTextSize(1);tft.setCursor(15,75);tft.print("Kurz tippen: Dark/Light");
    tft.setCursor(15,105);tft.print("3 Sekunden halten: Neustart");
    tft.setCursor(15,160);tft.print("Neustart mit Bestaetigung durch Halten");
  } else {cfg.darkMode=!cfg.darkMode;saveSettings();screenPage=0;drawLive();}
}

void setup() {
  Serial.begin(115200);
  bootMs=millis();
  loadSettings();

  tft.init(); tft.setRotation(1); tft.fillScreen(bgColor());
  tft.setTextColor(fgColor()); tft.setTextSize(2); tft.setCursor(10,20);
  tft.println("Energy Monitor");
  tft.setTextSize(1); tft.println("Starte...");

  touchSPI.begin(TOUCH_CLK,TOUCH_MISO,TOUCH_MOSI,TOUCH_CS);
  touch.begin(touchSPI); touch.setRotation(1);

  sdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdSPI,20000000);
  if(!sdReady) Serial.println("SD-Initialisierung fehlgeschlagen");

  connectWiFi();
  setupWeb();
  recordEvent("INFO","System","Energy Manager V2.4.2 gestartet");
  if(!sdReady) Serial.println("Warnung: Fehlerprotokoll ohne SD nicht persistent");
  if(WiFi.status()==WL_CONNECTED) fetchReader();
  loadDayBaseline();
  updateSdUsage();
  drawLive();
  // Task-Watchdog: erkennt blockierte Hauptschleifen.
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtCfg = { .timeout_ms = 15000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(15,true);
#endif
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  // HTTP-Antwort und SD-Schreibvorgang vor Neustart abschliessen.
  if(restartPending && millis()-restartRequestedMs>=1000UL) {
    recordEvent("WARN","System","Neustart: "+restartReason);
    delay(80);
    ESP.restart();
  }
  // Beide Quellen 5 Minuten ohne Erfolg: Neustart mit 30-Minuten-Sperre.
  // Eine einzelne ausgefallene Quelle loest keinen Neustart aus.
  if(millis()-bootMs>DATA_STALE_RESTART_MS &&
     millis()-(lastSuccessfulAnyMs?lastSuccessfulAnyMs:bootMs)>DATA_STALE_RESTART_MS &&
     WiFi.status()==WL_CONNECTED) {
    time_t now=time(nullptr);
    if(now>1700000000) {
      prefs.begin("energy-restart",false);
      uint32_t last=(uint32_t)prefs.getULong("lastAuto",0);
      if(!last || (uint32_t)now-last>=RESTART_COOLDOWN_SECONDS) {
        prefs.putULong("lastAuto",(uint32_t)now);
        prefs.end();
        requestRestart("5 Minuten ohne erfolgreiche Datenquelle");
      } else prefs.end();
    }
  }
  if(time(nullptr)>100000)loadDayBaseline();
  bool wifiConnected=WiFi.status()==WL_CONNECTED;
  if(wifiConnected!=wifiWasConnected){
    wifiWasConnected=wifiConnected;
    recordEvent(wifiConnected?"INFO":"ERROR","WLAN",wifiConnected?"Verbindung hergestellt: "+WiFi.localIP().toString():"Verbindung unterbrochen");
    if(wifiConnected) configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.google.com");
  }
  if(!wifiConnected && cfg.wifiSsid.length() && millis()-lastWiFiReconnect>15000UL){
    lastWiFiReconnect=millis();WiFi.reconnect();
  }

  if(millis()-lastPollMs >= (uint32_t)cfg.pollSeconds*1000) {
    lastPollMs=millis();
    fetchReader();
    if(liveData.valid) {
      if(isfinite(liveData.importW)) {importPowerSum+=liveData.importW; importPowerMax=max(importPowerMax,liveData.importW);}
      if(isfinite(liveData.exportW)) {exportPowerSum+=liveData.exportW; exportPowerMax=max(exportPowerMax,liveData.exportW);}
      sampleCount++;
      if(isfinite(liveData.rawPowerW)) {rawPowerSum+=liveData.rawPowerW;rawCount++;}
    }
  }
  if(cfg.solarHost.length() && millis()-lastSolarPollMs >= (uint32_t)cfg.solarPollSeconds*1000UL) {
    lastSolarPollMs=millis(); if(fetchSolar() && isfinite(liveData.solarPowerW)) {
      solarPowerSum+=liveData.solarPowerW; solarCount++;
    }
  }
  if(lastReaderSuccess>0 || lastSolarSuccess>0) {
    // Erfolg wird anhand eines geaenderten Zeitstempels erkannt.
    static time_t seenReader=0,seenSolar=0;
    if(lastReaderSuccess!=seenReader || lastSolarSuccess!=seenSolar) {
      seenReader=lastReaderSuccess;seenSolar=lastSolarSuccess;
      lastSuccessfulAnyMs=millis();
    }
  }
  if(millis()-lastLogMs>=60000) { lastLogMs=millis(); logMinute(); }
  if(millis()-lastLogPruneMs>=3600000UL || lastLogPruneMs==0){
    lastLogPruneMs=millis();pruneErrorLogs();
  }
  // Startup reads CSV incrementally once; afterward all graphs use RAM only.
  serviceHistoryStartup();
  handleTouch();
  if(millis()-lastDrawMs>2000) {
    lastDrawMs=millis();
    if(screenPage==0) drawLive();
    // Grafik nur bei neuen Daten oder spaetestens alle 60 Sekunden zeichnen.
    // Das verhindert tausende TFT-Zeichenoperationen alle 2 Sekunden.
    else if(screenPage==1) {
      static uint32_t lastGraphDrawMs=0, lastGraphDataMs=0;
      if(lastGraphDrawMs==0 || graphLoadedMs!=lastGraphDataMs ||
         millis()-lastGraphDrawMs>=60000UL) {
        drawGraph();
        lastGraphDrawMs=millis();lastGraphDataMs=graphLoadedMs;
      }
    }
  }
  delay(5);
}

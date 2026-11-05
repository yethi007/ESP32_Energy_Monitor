/*
  ESP32 Energy Monitor V2.2 - ESP32-2432S028 (CYD)
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
#include "cyd_config.h"

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
} cfg;

struct MeterData {
  double importKWh = NAN;
  double exportKWh = NAN;
  double importW = NAN;
  double exportW = NAN;
  time_t updated = 0;
  bool valid = false;
  double rawPowerW = NAN, solarPowerW = NAN, solarTotalKWh = NAN;
  bool solarValid = false;
} liveData;

uint32_t lastPollMs = 0, lastSolarPollMs = 0, lastLogMs = 0, lastDrawMs = 0;
double importPowerSum = 0, exportPowerSum = 0, rawPowerSum = 0, solarPowerSum = 0;
uint32_t sampleCount = 0, rawCount = 0, solarCount = 0;
double importPowerMax = 0, exportPowerMax = 0;
bool sdReady = false, readerOnline = false;
bool wifiWasConnected=false;
uint32_t lastWiFiReconnect=0, lastLogPruneMs=0;
time_t lastReaderSuccess=0, lastSolarSuccess=0;
String readerError="", solarError="";
// Error log: transitions and a maximum of one repeated identical error per 10 minutes.
String previousEventKey="";
uint32_t previousEventMs=0;
uint32_t lastReaderErrorMs=0, lastSolarErrorMs=0;
String lastReaderErrorKey="", lastSolarErrorKey="";

String lastError = "Noch keine Abfrage";
int screenPage = 0;
double dayImportStart=NAN, daySolarStart=NAN;
int dayKey=-1;
uint16_t bgColor(){return cfg.darkMode?TFT_BLACK:TFT_WHITE;}
uint16_t fgColor(){return cfg.darkMode?TFT_WHITE:TFT_BLACK;}

uint32_t lastTouchMs = 0;

String jsonEscape(const String &s) {
  String o; for (size_t i=0;i<s.length();i++) {
    char c=s[i]; if(c=='"' || c=='\\') o+='\\';
    if(c=='\n') o+="\\n"; else if(c!='\r') o+=c;
  } return o;
}

String timestampString(time_t t);
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
  s += ",\"rssi\":"+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0);
  s += ",\"reader_last_ok\":\""+timestampString(lastReaderSuccess)+"\"";
  s += ",\"solar_last_ok\":\""+timestampString(lastSolarSuccess)+"\"";
  s += ",\"reader_error\":\""+jsonEscape(readerError)+"\"";
  s += ",\"solar_error\":\""+jsonEscape(solarError)+"\"";
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
<div class="card"><small>Solarleistung</small><div class="v" id="sw">-- W</div></div>
<div class="card"><small>Solarerzeugung gesamt</small><div class="v" id="sk">-- kWh</div></div>
<div class="card"><small>Netzbezug heute</small><div class="v" id="di">-- kWh</div></div>
<div class="card"><small>Solarerzeugung heute</small><div class="v" id="ds">-- kWh</div></div>
</div><div class="card" style="margin-top:12px"><h2>Leistung letzte 24 Stunden</h2><canvas id="graph" width="900" height="300" style="width:100%;height:300px"></canvas><small>Netz (blau) und PV (gelb), Minutenwerte</small></div>
<p id="updated" class="muted"></p><p><a href="/config">Einstellungen</a> <a href="/api/status">JSON API</a> <a href="/logs">Fehlerprotokoll</a></p>
<div class="card"><h3>Netzwerk und Datenquellen</h3><div id="diagnostics">Lade Status…</div></div>
<script>
async function upd(){try{let r=await fetch('/api/status');let d=await r.json();
document.getElementById('iw').textContent=d.import_w==null?'-- W':d.import_w.toFixed(0)+' W';
document.getElementById('ew').textContent=d.export_w==null?'-- W':d.export_w.toFixed(0)+' W';
document.getElementById('ik').textContent=(d.import_kwh??0).toFixed(3)+' kWh';
document.getElementById('ek').textContent=(d.export_kwh??0).toFixed(3)+' kWh';
document.getElementById('sw').textContent=d.solar_w==null?'-- W':d.solar_w.toFixed(0)+' W';
document.getElementById('sk').textContent=d.solar_kwh==null?'-- kWh':d.solar_kwh.toFixed(3)+' kWh';
document.getElementById('di').textContent=d.today_import_kwh==null?'-- kWh':d.today_import_kwh.toFixed(3)+' kWh';
document.getElementById('ds').textContent=d.today_solar_kwh==null?'-- kWh':d.today_solar_kwh.toFixed(3)+' kWh';
document.getElementById('state').textContent=(d.online?'Lesekopf verbunden':'Lesekopf nicht verbunden')+' · WLAN '+(d.wifi?'verbunden':'getrennt')+' · SD '+(d.sd?'bereit':'Fehler');
document.getElementById('updated').textContent='Letzte Aktualisierung: '+d.updated+(d.error?' · '+d.error:'');
document.getElementById('diagnostics').textContent='ESP32: '+d.ip+' · WLAN '+d.rssi+' dBm · Lesekopf zuletzt: '+d.reader_last_ok+' · Solar zuletzt: '+d.solar_last_ok+' · Lesekopf-Fehler: '+(d.reader_error||'keiner')+' · Solar-Fehler: '+(d.solar_error||'keiner');
}catch(e){document.getElementById('state').textContent='Verbindung zum ESP32 fehlgeschlagen';}}
async function drawChart(){try{
const r=await fetch('/api/chart24');const d=await r.json();const pts=d.points||[];
const c=document.getElementById('graph'),ctx=c.getContext('2d');const W=c.width,H=c.height;
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
}catch(e){}}
upd();drawChart();setInterval(upd,1000);setInterval(drawChart,60000);
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

const int GRAPH_N=1440;
float graphNet[GRAPH_N];
float graphSolar[GRAPH_N];
uint32_t graphTime[GRAPH_N];
int graphCount=0;
uint32_t graphLoadedMs=0;

void loadGraph24() {
  graphCount=0;
  if(!sdReady || time(nullptr)<100000) return;
  time_t now=time(nullptr),cutoff=now-86400L;
  for(int day=1;day>=0;day--){
    File f=SD.open(dailyPath(now-(time_t)day*86400L),FILE_READ);
    if(!f)continue;
    f.readStringUntil('\n');
    while(f.available()){
      String line=f.readStringUntil('\n');
      if((graphCount%80)==0) {yield();}
      int a=line.indexOf(','),b=line.indexOf(',',a+1),c=line.indexOf(',',b+1),d=line.indexOf(',',c+1),e=line.indexOf(',',d+1);
      if(a<0||b<0||c<0||d<0||e<0)continue;
      String stamp=line.substring(0,a);
      int yy,mo,dd,hh,mm,ss;
      if(sscanf(stamp.c_str(),"%d-%d-%d-%d-%d-%d",&yy,&mo,&dd,&hh,&mm,&ss)!=6)continue;
      struct tm t={0};t.tm_year=yy-1900;t.tm_mon=mo-1;t.tm_mday=dd;t.tm_hour=hh;t.tm_min=mm;t.tm_sec=ss;t.tm_isdst=-1;
      time_t ts=mktime(&t);
      if(ts<cutoff||ts>now)continue;
      if(graphCount>=GRAPH_N)continue;
      String nw=line.substring(c+1,d),sw=line.substring(d+1,e);
      graphTime[graphCount]=(uint32_t)ts;
      graphNet[graphCount]=(nw=="null")?NAN:nw.toFloat();
      graphSolar[graphCount]=(sw=="null")?NAN:sw.toFloat();
      graphCount++;
    }
    f.close();
  }
  graphLoadedMs=millis();
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
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json","{");
  server.sendContent("\"points\":[");
  for(int i=0;i<graphCount;i++) {
    if(i)server.sendContent(",");
    server.sendContent("{\"t\":"+String(graphTime[i])+",\"w\":"+jsonNum(graphNet[i],1)+",\"solar\":"+jsonNum(graphSolar[i],1)+"}");
    if(i%30==0) yield();
  }
  server.sendContent("]}");server.sendContent("");
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

void drawLive() {
  tft.fillScreen(bgColor());tft.setTextColor(fgColor(),bgColor());
  tft.setTextSize(2);tft.setCursor(8,7);tft.print("ENERGY MONITOR V2.2");
  tft.setTextSize(1);tft.setCursor(8,32);
  tft.setTextColor(readerOnline?TFT_GREEN:TFT_RED);tft.print(readerOnline?"Lesekopf online":"Lesekopf offline");
  tft.setTextColor(fgColor());tft.setTextSize(2);
  tft.setCursor(8,53);tft.printf("Netz: %.0f W",isfinite(liveData.rawPowerW)?liveData.rawPowerW:0.0);
  tft.setCursor(8,80);tft.printf("PV: %.0f W",isfinite(liveData.solarPowerW)?liveData.solarPowerW:0.0);
  tft.setTextSize(1);tft.setCursor(8,118);tft.printf("Bezug: %.3f kWh",isfinite(liveData.importKWh)?liveData.importKWh:0.0);
  tft.setCursor(8,136);tft.printf("Einspeisung: %.3f kWh",isfinite(liveData.exportKWh)?liveData.exportKWh:0.0);
  tft.setCursor(8,154);tft.printf("PV gesamt: %.3f kWh",isfinite(liveData.solarTotalKWh)?liveData.solarTotalKWh:0.0);
  tft.setCursor(8,174);tft.printf("Heute Bezug: %s kWh",jsonNum(todayImport(),3).c_str());
  tft.setCursor(8,188);tft.printf("Heute PV: %s kWh",jsonNum(todaySolar(),3).c_str());
  tft.setCursor(8,203);tft.printf("WLAN %s | SD %s",WiFi.status()==WL_CONNECTED?"OK":"OFF",sdReady?"OK":"OFF");
  tft.setCursor(145,203);tft.print(WiFi.localIP().toString());
  tft.setCursor(255,32);tft.printf("%d dBm",WiFi.status()==WL_CONNECTED?WiFi.RSSI():0);
  tft.setTextColor(TFT_CYAN);tft.setCursor(8,225);tft.print("Tippen: Grafik / Modus");
}

void handleTouch() {
  if(millis()-lastTouchMs<400 || !touch.touched())return;
  touch.getPoint();lastTouchMs=millis();screenPage=(screenPage+1)%3;
  if(screenPage==1){if(!graphLoadedMs)loadGraph24();drawGraph();}
  else if(screenPage==2){
    cfg.darkMode=!cfg.darkMode;saveSettings();screenPage=0;drawLive();
  }else drawLive();
}

void setup() {
  Serial.begin(115200);
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
  recordEvent("INFO","System","Energy Monitor V2.2 gestartet");
  if(!sdReady) Serial.println("Warnung: Fehlerprotokoll ohne SD nicht persistent");
  if(WiFi.status()==WL_CONNECTED) fetchReader();
  loadDayBaseline();
  drawLive();
}

void loop() {
  server.handleClient();
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
  if(millis()-lastLogMs>=60000) { lastLogMs=millis(); logMinute(); }
  if(millis()-lastLogPruneMs>=3600000UL || lastLogPruneMs==0){
    lastLogPruneMs=millis();pruneErrorLogs();
  }
  // Refresh graph cache every five minutes in the main loop; web requests never scan CSVs.
  if(sdReady && time(nullptr)>1700000000 &&
     (graphLoadedMs==0 || millis()-graphLoadedMs>300000UL)) {
    loadGraph24();
  }
  handleTouch();
  if(millis()-lastDrawMs>2000) {
    lastDrawMs=millis();
    if(screenPage==0) drawLive();
    else if(millis()-graphLoadedMs>300000) {loadGraph24();drawGraph();}
  }
  delay(5);
}

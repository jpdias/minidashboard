#include "logbuf.h"
#include "nettime.h"
#include "config.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

static WiFiUDP ntpUDP;
static NTPClient ntp(ntpUDP, "pool.ntp.org", 0, 600000);
static bool gSynced = false;
static unsigned long gLastSync = 0;
static unsigned long gLastAttempt = 0;

void time_begin() {
  ntp.begin();
}

bool time_is_synced() { return gSynced; }

// ESP8266 newlib tzset() needs a POSIX TZ string with DST rules, NOT an IANA
// name like "Europe/Lisbon". Map the friendly names we expose to POSIX here.
struct TzEntry { const char* name; const char* posix; };
static const TzEntry TZ_TABLE[] = {
  {"Europe/Lisbon",   "WET0WEST,M3.5.0/1,M10.5.0"},
  {"Europe/London",   "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Europe/Madrid",   "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Paris",    "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Berlin",   "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Rome",     "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Athens",   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"America/New_York","EST5EDT,M3.2.0,M11.1.0"},
  {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Denver",  "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"},
  {"America/Sao_Paulo","<-03>3"},
  {"Asia/Tokyo",      "JST-9"},
  {"Asia/Shanghai",   "CST-8"},
  {"Asia/Kolkata",    "IST-5:30"},
  {"Asia/Dubai",      "<+04>-4"},
  {"Australia/Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"UTC",             "UTC0"},
};

const char* tz_to_posix(const char* name) {
  for (unsigned i = 0; i < sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]); i++) {
    if (strcmp(name, TZ_TABLE[i].name) == 0) return TZ_TABLE[i].posix;
  }
  return name;  // assume caller already gave a POSIX string
}

int tz_count() { return sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]); }
const char* tz_name_at(int i) { return TZ_TABLE[i].name; }

void time_update() {
  bool ok = ntp.forceUpdate();
  gLastAttempt = millis();
  time_t epoch = ntp.getEpochTime();
  // Sanity: epoch must be after 2021-01-01 to count as a real sync.
  if (ok && epoch > 1609459200UL) {
    timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    gSynced = true;
    gLastSync = millis();
    mlog.printf("[TIME] NTP synced epoch=%lu\n", (unsigned long)epoch);
  } else {
    mlog.println("[TIME] NTP update failed");
  }
  const char* posix = tz_to_posix(cfg.tz);
  setenv("TZ", posix, 1);
  tzset();
  mlog.printf("[TIME] TZ '%s' -> POSIX '%s'\n", cfg.tz, posix);
}

// Periodic resync + retry. Call from loop().
void time_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (!gSynced) {
    if (now - gLastAttempt >= 15000) time_update();   // retry every 15s until first sync
    return;
  }
  unsigned long periodMs = (unsigned long)cfg.ntp_interval_min * 60000UL;
  if (periodMs < 60000UL) periodMs = 60000UL;
  if (now - gLastSync >= periodMs) time_update();
}

void time_now(int &h, int &m, int &s, int &dow, int &day, int &mon, int &yr) {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  h = t->tm_hour; m = t->tm_min; s = t->tm_sec;
  dow = t->tm_wday; day = t->tm_mday; mon = t->tm_mon + 1; yr = t->tm_year + 1900;
}

const char* dow_name(int d) {
  static const char* names[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  return names[d % 7];
}

time_t time_utc_now() { return time(nullptr); }

long time_tz_offset() {
  time_t now = time(nullptr);
  // gmtime() gives the UTC calendar time; mktime() re-interprets those fields as
  // LOCAL time, so mktime(gmtime(now)) == now - offset. Hence offset = now - that.
  struct tm g = *gmtime(&now);
  g.tm_isdst = -1;
  time_t asLocal = mktime(&g);
  return (long)difftime(now, asLocal);
}

// ---- shared HTTP GET (blocking) used by the simple wrappers ----
static bool http_get(const char* host, const char* url, String &body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  if (!client.connect(host, 80)) return false;
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: miniDash\r\n" +
               "Connection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (!client.available() && millis() - t0 < 5000) yield();
  bool headersDone = false;
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (!headersDone && line.length() <= 1) headersDone = true;
    else if (headersDone) body += line;
  }
  client.stop();
  return body.length() > 0;
}

// Strip HTTP headers and return only the JSON body starting at '{'.
static String json_only(const String &body) {
  int i = body.indexOf('{');
  return i >= 0 ? body.substring(i) : String();
}

bool parse_weather_body(const String &body, Weather &w) {
  String json = json_only(body);
  if (json.length() == 0) { mlog.println("[WX] no JSON object"); return false; }
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, json.c_str())) { mlog.println("[WX] JSON parse error"); return false; }
  JsonObject cur = doc["current"];
  if (!cur) { mlog.println("[WX] no 'current'"); return false; }
  w.temp = cur["temperature_2m"].as<float>();
  w.humidity = cur["relative_humidity_2m"].as<int>();
  w.code = cur["weather_code"].as<int>();
  strncpy(w.desc, weather_icon(w.code), sizeof(w.desc) - 1);
  // Sun times from daily[0]; ISO "YYYY-MM-DDTHH:MM" -> take the "HH:MM" part.
  w.sunrise[0] = w.sunset[0] = 0;
  JsonObject d = doc["daily"];
  if (d) {
    const char* sr = d["sunrise"][0] | "";
    const char* ss = d["sunset"][0] | "";
    if (strlen(sr) >= 16) { strncpy(w.sunrise, sr + 11, 5); w.sunrise[5] = 0; }
    if (strlen(ss) >= 16) { strncpy(w.sunset, ss + 11, 5); w.sunset[5] = 0; }
  }
  w.valid = true;
  mlog.printf("[WX] OK temp=%.1f hum=%d code=%d (%s)\n", w.temp, w.humidity, w.code, w.desc);
  return true;
}

bool parse_forecast_body(const String &body, Forecast &f) {
  String json = json_only(body);
  if (json.length() == 0) { mlog.println("[FC] no JSON"); return false; }
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json.c_str())) { mlog.println("[FC] parse error"); return false; }
  JsonObject d = doc["daily"];
  if (!d) { mlog.println("[FC] no daily"); return false; }
  JsonArray code = d["weather_code"];
  JsonArray tmax = d["temperature_2m_max"];
  JsonArray tmin = d["temperature_2m_min"];
  int n = min((int)code.size(), 3);
  for (int i = 0; i < n; i++) {
    f.days[i].code = code[i];
    f.days[i].tmax = tmax[i];
    f.days[i].tmin = tmin[i];
    f.days[i].valid = true;
  }
  f.valid = (n > 0);
  mlog.printf("[FC] got %d days\n", n);
  return true;
}

bool parse_extip_body(const String &body, String &ip) {
  String json = json_only(body);
  if (json.length() == 0) { mlog.println("[IP] no JSON"); return false; }
  // Filter to only the "ip" field so buffer size is independent of response size.
  StaticJsonDocument<32> filter;
  filter["ip"] = true;
  DynamicJsonDocument doc(128);
  DeserializationError err = deserializeJson(doc, json.c_str(), DeserializationOption::Filter(filter));
  if (err) { mlog.printf("[IP] parse error: %s\n", err.c_str()); return false; }
  const char* p = doc["ip"] | "";
  if (strlen(p) < 7 || strchr(p, '.') == NULL) { mlog.printf("[IP] unexpected: '%s'\n", p); return false; }
  ip = p;
  mlog.printf("[IP] external=%s\n", ip.c_str());
  return true;
}

bool weather_fetch(float lat, float lon, Weather &w) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String host = "api.open-meteo.com";
  String url = "/v1/forecast?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code" +
               "&daily=sunrise,sunset&forecast_days=1&timezone=auto";
  mlog.printf("[WX] request: http://%s%s\n", host.c_str(), url.c_str());
  mlog.printf("[WX] lat=%.4f lon=%.4f\n", lat, lon);
  String body;
  if (!http_get(host.c_str(), url.c_str(), body)) { mlog.println("[WX] request failed"); return false; }
  mlog.printf("[WX] body len=%d\n", body.length());
  return parse_weather_body(body, w);
}

bool forecast_fetch(float lat, float lon, Forecast &f) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String host = "api.open-meteo.com";
  String url = "/v1/forecast?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
               "&forecast_days=4&timezone=auto";
  mlog.printf("[FC] request: http://%s%s\n", host.c_str(), url.c_str());
  String body;
  if (!http_get(host.c_str(), url.c_str(), body)) { mlog.println("[FC] request failed"); return false; }
  return parse_forecast_body(body, f);
}

String external_ip_fetch() {
  String host = "ipinfo.io";
  String url = "/json";
  String body;
  if (!http_get(host.c_str(), url.c_str(), body)) { mlog.println("[IP] request failed"); return String(); }
  String ip;
  if (!parse_extip_body(body, ip)) return String();
  return ip;
}

const char* weather_icon(int code) {
  // WMO weather interpretation codes (simplified)
  if (code == 0) return "Clear";
  if (code == 1) return "Mainly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code >= 45 && code <= 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow showers";
  if (code >= 95 && code <= 99) return "Thunderstorm";
  return "Unknown";
}


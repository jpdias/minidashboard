#include "nettime.h"
#include "config.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

static WiFiUDP ntpUDP;
static NTPClient ntp(ntpUDP, "pool.ntp.org", 0, 600000);

void time_begin() {
  ntp.begin();
}

void time_update() {
  ntp.update();
  // Push NTP epoch into the system clock so time()/localtime() work
  time_t epoch = ntp.getEpochTime();
  timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  setenv("TZ", cfg.tz, 1);
  tzset();
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

// ---- shared HTTP GET (blocking) used by the simple wrappers ----
static bool http_get(const char* host, const char* url, String &body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  if (!client.connect(host, 80)) return false;
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: miniTV\r\n" +
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
  if (json.length() == 0) { Serial.println("[WX] no JSON object"); return false; }
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, json.c_str())) { Serial.println("[WX] JSON parse error"); return false; }
  JsonObject cur = doc["current"];
  if (!cur) { Serial.println("[WX] no 'current'"); return false; }
  w.temp = cur["temperature_2m"].as<float>();
  w.humidity = cur["relative_humidity_2m"].as<int>();
  w.code = cur["weather_code"].as<int>();
  strncpy(w.desc, weather_icon(w.code), sizeof(w.desc) - 1);
  w.valid = true;
  Serial.printf("[WX] OK temp=%.1f hum=%d code=%d (%s)\n", w.temp, w.humidity, w.code, w.desc);
  return true;
}

bool parse_forecast_body(const String &body, Forecast &f) {
  String json = json_only(body);
  if (json.length() == 0) { Serial.println("[FC] no JSON"); return false; }
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json.c_str())) { Serial.println("[FC] parse error"); return false; }
  JsonObject d = doc["daily"];
  if (!d) { Serial.println("[FC] no daily"); return false; }
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
  Serial.printf("[FC] got %d days\n", n);
  return true;
}

bool parse_extip_body(const String &body, String &ip) {
  String json = json_only(body);
  if (json.length() == 0) { Serial.println("[IP] no JSON"); return false; }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, json.c_str())) { Serial.println("[IP] parse error"); return false; }
  const char* p = doc["ip"] | "";
  if (strlen(p) < 7 || strchr(p, '.') == NULL) { Serial.printf("[IP] unexpected: '%s'\n", p); return false; }
  ip = p;
  Serial.printf("[IP] external=%s\n", ip.c_str());
  return true;
}

bool weather_fetch(float lat, float lon, Weather &w) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String host = "api.open-meteo.com";
  String url = "/v1/forecast?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code";
  Serial.printf("[WX] request: http://%s%s\n", host.c_str(), url.c_str());
  Serial.printf("[WX] lat=%.4f lon=%.4f\n", lat, lon);
  String body;
  if (!http_get(host.c_str(), url.c_str(), body)) { Serial.println("[WX] request failed"); return false; }
  Serial.printf("[WX] body len=%d\n", body.length());
  return parse_weather_body(body, w);
}

bool forecast_fetch(float lat, float lon, Forecast &f) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String host = "api.open-meteo.com";
  String url = "/v1/forecast?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
               "&forecast_days=4&timezone=auto";
  Serial.printf("[FC] request: http://%s%s\n", host.c_str(), url.c_str());
  String body;
  if (!http_get(host.c_str(), url.c_str(), body)) { Serial.println("[FC] request failed"); return false; }
  return parse_forecast_body(body, f);
}

String external_ip_fetch() {
  String host = "api.ipify.org";
  String url = "/?format=text";
  String body;
  if (!http_get(host.c_str(), url.c_str(), body)) { Serial.println("[IP] request failed"); return String(); }
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


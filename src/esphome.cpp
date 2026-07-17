#include "logbuf.h"
#include "esphome.h"
#include "httpfsm.h"
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

// Sensor endpoints exposed by the ESPHome REST API (friendly-name slugs, URL-encoded in request).
static const char* EH_IDS[EH_MAX] = {
  "IKEA Air Quality PM2.5",
  "Temperature",
  "Pressure",
  "Humidity"
};

static EspHomeState gSensors[EH_MAX];
static HttpFsm http;
static unsigned long ehLast = 0;
static bool ehFirst = true;
static bool ehActive = false;
static int ehIdx = 0;
static const unsigned long EH_INTERVAL = 30000;  // full refresh every 30s

const EspHomeState& esphome_state(int i) { return gSensors[i]; }
int esphome_count() { return EH_MAX; }

// Minimal URL-encoder (handles spaces and reserved chars for the friendly-name slugs).
static String urlencode(const String &s) {
  String out = "";
  const char *hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') ||
        ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

void esphome_begin() {
  ehLast = 0;
  ehFirst = true;
  ehActive = false;
  ehIdx = 0;
  http.consume();
}

static void start_sensor() {
  String url = String("/sensor/") + urlencode(EH_IDS[ehIdx]);
  http.connectTimeout = 4000;
  http.begin(String(cfg.esphome_host), url);
}

static void parse_sensor(const String &raw) {
  String j = http_json_body(raw);
  DynamicJsonDocument doc(512);
  EspHomeState &s = gSensors[ehIdx];
  if (j.length() && !deserializeJson(doc, j.c_str())) {
    const char* nm = doc["name"] | "";
    if (strlen(nm) == 0) nm = EH_IDS[ehIdx];
    strncpy(s.name, nm, sizeof(s.name) - 1);
    strncpy(s.state, doc["state"] | "", sizeof(s.state) - 1);
    strncpy(s.uom, doc["uom"] | "", sizeof(s.uom) - 1);
    sanitize_ascii(s.name);
    sanitize_ascii(s.state);
    sanitize_ascii(s.uom);
    s.valid = true;
    mlog.printf("[EH] %s = %s %s\n", s.name, s.state, s.uom);
  } else {
    s.valid = false;
    mlog.printf("[EH] parse error %s\n", EH_IDS[ehIdx]);
  }
}

static void advance() {
  ehIdx++;
  if (ehIdx >= EH_MAX) {
    ehIdx = 0;
    ehLast = millis();
    ehActive = false;
  } else {
    start_sensor();
  }
}

void esphome_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.esphome_host[0] == 0) return;

  if (!ehActive) {
    if (ehFirst || millis() - ehLast >= EH_INTERVAL) {
      ehFirst = false;
      ehActive = true;
      ehIdx = 0;
      start_sensor();
    }
    return;
  }

  http.tick();
  if (http.done()) {
    String raw = http.body();
    http.consume();
    parse_sensor(raw);
    advance();
  } else if (http.failed()) {
    mlog.printf("[EH] fetch fail %s\n", EH_IDS[ehIdx]);
    http.consume();
    gSensors[ehIdx].valid = false;
    advance();
  }
}

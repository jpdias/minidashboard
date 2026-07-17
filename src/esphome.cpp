#include "logbuf.h"
#include "esphome.h"
#include "httpfsm.h"
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

// Sensor slug + display label, parsed from cfg.esphome_sensors ("slug=label,...").
static char ehSlug[EH_MAX][48];
static char ehLabel[EH_MAX][24];
static int ehCount = 0;
static bool ehChanged = false;

static EspHomeState gSensors[EH_MAX];
static HttpFsm http;
static unsigned long ehLast = 0;
static bool ehFirst = true;
static bool ehActive = false;
static int ehIdx = 0;
static const unsigned long EH_INTERVAL = 30000;  // full refresh every 30s

const EspHomeState& esphome_state(int i) { return gSensors[i]; }
int esphome_count() { return ehCount; }
bool esphome_updated() { bool c = ehChanged; ehChanged = false; return c; }

// Parse "slug=label,slug2=label2" from config into ehSlug/ehLabel.
static void parse_sensor_config() {
  ehCount = 0;
  String cfgStr = cfg.esphome_sensors;
  int start = 0;
  while (start < (int)cfgStr.length() && ehCount < EH_MAX) {
    int comma = cfgStr.indexOf(',', start);
    if (comma < 0) comma = cfgStr.length();
    String pair = cfgStr.substring(start, comma);
    pair.trim();
    start = comma + 1;
    if (pair.length() == 0) continue;
    int eq = pair.indexOf('=');
    String slug = (eq >= 0) ? pair.substring(0, eq) : pair;
    String label = (eq >= 0) ? pair.substring(eq + 1) : pair;
    slug.trim(); label.trim();
    if (slug.length() == 0) continue;
    strncpy(ehSlug[ehCount], slug.c_str(), sizeof(ehSlug[0]) - 1);
    ehSlug[ehCount][sizeof(ehSlug[0]) - 1] = 0;
    strncpy(ehLabel[ehCount], label.c_str(), sizeof(ehLabel[0]) - 1);
    ehLabel[ehCount][sizeof(ehLabel[0]) - 1] = 0;
    ehCount++;
  }
  mlog.printf("[EH] configured %d sensors\n", ehCount);
}

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
  parse_sensor_config();
  ehLast = 0;
  ehFirst = true;
  ehActive = false;
  ehIdx = 0;
  http.consume();
}

static void start_sensor() {
  String url = String("/sensor/") + urlencode(ehSlug[ehIdx]);
  http.connectTimeout = 4000;
  http.begin(String(cfg.esphome_host), url);
}

static void parse_sensor(const String &raw) {
  String j = http_json_body(raw);
  DynamicJsonDocument doc(512);
  EspHomeState &s = gSensors[ehIdx];
  if (j.length() && !deserializeJson(doc, j.c_str())) {
    char oldState[sizeof(s.state)]; strncpy(oldState, s.state, sizeof(oldState));
    bool wasValid = s.valid;
    // Prefer the configured label; fall back to JSON name, then slug.
    const char* nm = ehLabel[ehIdx];
    if (strlen(nm) == 0) nm = doc["name"] | "";
    if (strlen(nm) == 0) nm = ehSlug[ehIdx];
    strncpy(s.name, nm, sizeof(s.name) - 1);
    strncpy(s.state, doc["state"] | "", sizeof(s.state) - 1);
    strncpy(s.uom, doc["uom"] | "", sizeof(s.uom) - 1);
    sanitize_ascii(s.name);
    sanitize_ascii(s.state);
    sanitize_ascii(s.uom);
    s.valid = true;
    if (!wasValid || strcmp(oldState, s.state) != 0) ehChanged = true;
    mlog.printf("[EH] %s = %s %s\n", s.name, s.state, s.uom);
  } else {
    if (s.valid) ehChanged = true;
    s.valid = false;
    mlog.printf("[EH] parse error %s\n", ehSlug[ehIdx]);
  }
}

static void advance() {
  ehIdx++;
  if (ehIdx >= ehCount) {
    ehIdx = 0;
    ehLast = millis();
    ehActive = false;
  } else {
    start_sensor();
  }
}

void esphome_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.esphome_host[0] == 0 || ehCount == 0) return;

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
    mlog.printf("[EH] fetch fail %s\n", ehSlug[ehIdx]);
    http.consume();
    if (gSensors[ehIdx].valid) ehChanged = true;
    gSensors[ehIdx].valid = false;
    advance();
  }
}

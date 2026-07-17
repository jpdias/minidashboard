#include "logbuf.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

Config cfg;

static const char* CONFIG_PATH = "/config.json";

// Legacy EEPROM struct (v1 layout) for one-time migration into JSON.
struct LegacyConfig {
  char wifi_ssid[33];
  char wifi_pass[65];
  float lat;
  float lon;
  char tz[32];
  int weather_interval;
  bool show_metrics;
  char monitors[MONITOR_MAX][MONITOR_LEN];
  char esphome_host[MONITOR_LEN];
};

static void apply_defaults() {
  cfg = Config{};
  strncpy(cfg.monitors[0], "google.com", MONITOR_LEN - 1);
  strncpy(cfg.monitors[1], "github.com", MONITOR_LEN - 1);
  strncpy(cfg.monitors[2], "open-meteo.com", MONITOR_LEN - 1);
  strncpy(cfg.esphome_host, "ikea-hack.lan", MONITOR_LEN - 1);
}

static bool load_json() {
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { mlog.printf("[CFG] json parse error: %s\n", err.c_str()); return false; }

  strncpy(cfg.wifi_ssid, doc["wifi_ssid"] | "", sizeof(cfg.wifi_ssid) - 1);
  strncpy(cfg.wifi_pass, doc["wifi_pass"] | "", sizeof(cfg.wifi_pass) - 1);
  cfg.lat = doc["lat"] | 0.0f;
  cfg.lon = doc["lon"] | 0.0f;
  strncpy(cfg.tz, doc["tz"] | "Europe/Lisbon", sizeof(cfg.tz) - 1);
  cfg.weather_interval = doc["weather_interval"] | 600;
  cfg.show_metrics = doc["show_metrics"] | true;
  strncpy(cfg.esphome_host, doc["esphome_host"] | "", sizeof(cfg.esphome_host) - 1);
  strncpy(cfg.esphome_sensors, doc["esphome_sensors"] | cfg.esphome_sensors, sizeof(cfg.esphome_sensors) - 1);
  cfg.night_start = doc["night_start"] | 23;
  cfg.night_end = doc["night_end"] | 7;
  cfg.ntp_interval_min = doc["ntp_interval_min"] | 60;
  for (int i = 0; i < MONITOR_MAX; i++) cfg.monitors[i][0] = 0;
  JsonArray mons = doc["monitors"];
  int mi = 0;
  for (JsonVariant v : mons) {
    if (mi >= MONITOR_MAX) break;
    strncpy(cfg.monitors[mi++], v.as<const char*>(), MONITOR_LEN - 1);
  }
  return true;
}

// Try to import old EEPROM config into cfg. Returns true if a valid one existed.
static bool import_eeprom() {
  EEPROM.begin(CONFIG_SIZE);
  uint16_t magic = 0;
  uint8_t ver = 0;
  EEPROM.get(0, magic);
  EEPROM.get(2, ver);
  if (magic != CONFIG_MAGIC) { EEPROM.end(); return false; }
  LegacyConfig lc;
  EEPROM.get(4, lc);
  EEPROM.end();

  apply_defaults();  // start from defaults so new fields are populated
  strncpy(cfg.wifi_ssid, lc.wifi_ssid, sizeof(cfg.wifi_ssid) - 1);
  strncpy(cfg.wifi_pass, lc.wifi_pass, sizeof(cfg.wifi_pass) - 1);
  cfg.lat = lc.lat;
  cfg.lon = lc.lon;
  strncpy(cfg.tz, lc.tz, sizeof(cfg.tz) - 1);
  cfg.weather_interval = lc.weather_interval;
  cfg.show_metrics = lc.show_metrics;
  strncpy(cfg.esphome_host, lc.esphome_host, sizeof(cfg.esphome_host) - 1);
  for (int i = 0; i < MONITOR_MAX; i++) {
    lc.monitors[i][MONITOR_LEN - 1] = 0;
    strncpy(cfg.monitors[i], lc.monitors[i], MONITOR_LEN - 1);
  }
  mlog.println("[CFG] imported legacy EEPROM config");
  return true;
}

void config_load() {
  if (!LittleFS.begin()) mlog.println("[CFG] LittleFS mount failed");

  if (load_json()) {
    mlog.println("[CFG] loaded config.json");
    return;
  }
  if (import_eeprom()) {
    config_save();  // persist migrated config as JSON
    return;
  }
  mlog.println("[CFG] no config found, using defaults");
  apply_defaults();
  config_save();
}

void config_save() {
  DynamicJsonDocument doc(2048);
  doc["wifi_ssid"] = cfg.wifi_ssid;
  doc["wifi_pass"] = cfg.wifi_pass;
  doc["lat"] = cfg.lat;
  doc["lon"] = cfg.lon;
  doc["tz"] = cfg.tz;
  doc["weather_interval"] = cfg.weather_interval;
  doc["show_metrics"] = cfg.show_metrics;
  doc["esphome_host"] = cfg.esphome_host;
  doc["esphome_sensors"] = cfg.esphome_sensors;
  doc["night_start"] = cfg.night_start;
  doc["night_end"] = cfg.night_end;
  doc["ntp_interval_min"] = cfg.ntp_interval_min;
  JsonArray mons = doc.createNestedArray("monitors");
  for (int i = 0; i < MONITOR_MAX; i++) {
    if (cfg.monitors[i][0]) mons.add(cfg.monitors[i]);
  }

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) { mlog.println("[CFG] save open failed"); return; }
  serializeJson(doc, f);
  f.close();
  mlog.println("[CFG] saved config.json");
}

void config_reset() {
  apply_defaults();
  config_save();
}

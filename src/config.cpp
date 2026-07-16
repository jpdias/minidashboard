#include "config.h"

Config cfg;

void config_load() {
  EEPROM.begin(CONFIG_SIZE);
  uint16_t magic = 0;
  uint8_t ver = 0;
  EEPROM.get(0, magic);
  EEPROM.get(2, ver);
  if (magic != CONFIG_MAGIC || ver != CONFIG_VERSION) {
    config_reset();
    return;
  }
  int off = 4;
  EEPROM.get(off, cfg); off += sizeof(Config);
  EEPROM.end();
}

void config_save() {
  EEPROM.begin(CONFIG_SIZE);
  EEPROM.put(0, CONFIG_MAGIC);
  EEPROM.put(2, (uint8_t)CONFIG_VERSION);
  int off = 4;
  EEPROM.put(off, cfg); off += sizeof(Config);
  EEPROM.commit();
  EEPROM.end();
}

void config_reset() {
  cfg = Config{};
  strncpy(cfg.monitors[0], "google.com", MONITOR_LEN - 1);
  strncpy(cfg.monitors[1], "github.com", MONITOR_LEN - 1);
  strncpy(cfg.monitors[2], "open-meteo.com", MONITOR_LEN - 1);
  strncpy(cfg.esphome_host, "ikea-hack.lan", MONITOR_LEN - 1);
  config_save();
}

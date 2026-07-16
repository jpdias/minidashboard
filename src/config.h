#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

#define CONFIG_MAGIC   0x4D54
#define CONFIG_VERSION 1
#define CONFIG_SIZE    1024
#define MONITOR_MAX    4
#define MONITOR_LEN    64

struct Config {
  char wifi_ssid[33] = {0};
  char wifi_pass[65] = {0};
  float lat = 0.0f;
  float lon = 0.0f;
  char tz[32] = "Europe/Lisbon";
  int   weather_interval = 600;  // seconds
  bool  show_metrics = true;
  char  monitors[MONITOR_MAX][MONITOR_LEN] = {0};
  char  esphome_host[MONITOR_LEN] = {0};
};

extern Config cfg;

void config_load();
void config_save();
void config_reset();

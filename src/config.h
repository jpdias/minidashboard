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

#define ESPHOME_SENSORS_LEN 192
#define SCREEN_MAX 7   // Clock, ESPHome, Forecast, Network, Detail, Monitors, Flight

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
  // slug=label pairs, comma separated (e.g. "Temperature=Temp,Humidity=Hum")
  char  esphome_sensors[ESPHOME_SENSORS_LEN] = "IKEA Air Quality PM2.5=Air,Temperature=Temp,Pressure=Press,Humidity=Hum";
  int   night_start = 23;         // hour screen turns off
  int   night_end = 7;            // hour screen turns on
  int   ntp_interval_min = 60;    // NTP resync period (minutes)
  int   flight_range = 25;        // flight radar range in nm (0 disables screen)
  bool  screen_enabled[SCREEN_MAX] = { true, true, true, true, true, true, true };
};

extern Config cfg;

void config_load();
void config_save();
void config_reset();

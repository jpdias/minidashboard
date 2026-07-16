#include "netfsm.h"
#include "config.h"
#include <WiFiClient.h>

enum NetState { NET_IDLE, NET_CONN, NET_WAIT, NET_READ, NET_NEXT };
enum NetTask  { TASK_WEATHER, TASK_FORECAST, TASK_EXTIP };

static Weather gWeather;
static Forecast gForecast;
static String gExtIp = "";
static bool gUpdated = false;

static NetState netState = NET_IDLE;
static NetTask netTask = TASK_WEATHER;
static unsigned long netInterval = 600000;
static unsigned long netLastCycle = 0;
static unsigned long netTimer = 0;
static WiFiClient netClient;
static String netHost = "";
static String netUrl = "";
static String netBody = "";
static bool netStarted = false;

Weather& net_weather() { return gWeather; }
Forecast& net_forecast() { return gForecast; }
String net_extip() { return gExtIp; }

bool netfsm_updated() {
  if (gUpdated) { gUpdated = false; return true; }
  return false;
}

void netfsm_begin(unsigned long intervalMs) {
  netInterval = intervalMs;
  netLastCycle = 0;        // force immediate first cycle
  netState = NET_IDLE;
  netStarted = false;
}

static void net_start_task() {
  netBody = "";
  netClient.stop();
  switch (netTask) {
    case TASK_WEATHER:
      netHost = "api.open-meteo.com";
      netUrl = "/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code";
      break;
    case TASK_FORECAST:
      netHost = "api.open-meteo.com";
      netUrl = "/v1/forecast?latitude=" + String(cfg.lat, 4) +
               "&longitude=" + String(cfg.lon, 4) +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
               "&forecast_days=4&timezone=auto";
      break;
    case TASK_EXTIP:
      netHost = "api.ipify.org";
      netUrl = "/?format=text";
      break;
  }
  Serial.printf("[NET] start %d -> %s%s\n", netTask, netHost.c_str(), netUrl.c_str());
  netState = NET_CONN;
  netTimer = millis();
}

void netfsm_tick() {
  if (WiFi.status() != WL_CONNECTED) return;

  switch (netState) {
    case NET_IDLE:
      if (millis() - netLastCycle >= netInterval) {
        netTask = TASK_WEATHER;
        net_start_task();
      }
      break;

    case NET_CONN:
      if (netClient.connect(netHost.c_str(), 80)) {
        netClient.print(String("GET ") + netUrl + " HTTP/1.1\r\n" +
                        "Host: " + netHost + "\r\n" +
                        "User-Agent: miniTV\r\n" +
                        "Connection: close\r\n\r\n");
        netState = NET_WAIT;
        netTimer = millis();
      } else if (millis() - netTimer > 4000) {
        Serial.printf("[NET] connect fail %d\n", netTask);
        netState = NET_NEXT;   // skip this task
      }
      break;

    case NET_WAIT:
      if (netClient.available()) {
        netState = NET_READ;
        netTimer = millis();
      } else if (millis() - netTimer > 5000) {
        Serial.printf("[NET] timeout %d\n", netTask);
        netState = NET_NEXT;
      }
      break;

    case NET_READ:
      // Accumulate non-blocking
      while (netClient.available()) {
        netBody += (char)netClient.read();
      }
      if (!netClient.connected() && !netClient.available()) {
        netClient.stop();
        // Parse based on task
        if (netTask == TASK_WEATHER)      { parse_weather_body(netBody, gWeather); gUpdated = true; }
        else if (netTask == TASK_FORECAST) { parse_forecast_body(netBody, gForecast); gUpdated = true; }
        else if (netTask == TASK_EXTIP) {
          String ip;
          if (parse_extip_body(netBody, ip) && ip.length()) gExtIp = ip;
          gUpdated = true;
        }
        netState = NET_NEXT;
      } else if (millis() - netTimer > 6000) {
        Serial.printf("[NET] read stall %d\n", netTask);
        netClient.stop();
        netState = NET_NEXT;
      }
      break;

    case NET_NEXT:
      if (netTask == TASK_WEATHER) netTask = TASK_FORECAST;
      else if (netTask == TASK_FORECAST) netTask = TASK_EXTIP;
      else {  // finished EXTIP
        netLastCycle = millis();
        gUpdated = true;
        netState = NET_IDLE;
        Serial.println("[NET] cycle complete");
      }
      if (netState != NET_IDLE) net_start_task();
      break;
  }
}

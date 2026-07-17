#include "logbuf.h"
#include "netfsm.h"
#include "config.h"
#include "httpfsm.h"
#include <ESP8266WiFi.h>

enum NetTask { TASK_WEATHER, TASK_FORECAST, TASK_EXTIP };

static Weather gWeather;
static Forecast gForecast;
static String gExtIp = "";
static bool gUpdated = false;
static bool gFirstDone = false;

static HttpFsm http;
static NetTask netTask = TASK_WEATHER;
static unsigned long netInterval = 600000;
static unsigned long netLastCycle = 0;
static bool netFirst = true;
static bool netActive = false;

Weather& net_weather() { return gWeather; }
Forecast& net_forecast() { return gForecast; }
String net_extip() { return gExtIp; }

bool netfsm_updated() {
  if (gUpdated) { gUpdated = false; return true; }
  return false;
}

bool netfsm_first_done() { return gFirstDone; }

void netfsm_begin(unsigned long intervalMs) {
  netInterval = intervalMs;
  netLastCycle = 0;
  netFirst = true;
  netActive = false;
  http.consume();
}

static void start_task(NetTask t) {
  String host, url;
  switch (t) {
    case TASK_WEATHER:
      host = "api.open-meteo.com";
      url = "/v1/forecast?latitude=" + String(cfg.lat, 4) +
            "&longitude=" + String(cfg.lon, 4) +
            "&current=temperature_2m,relative_humidity_2m,weather_code";
      break;
    case TASK_FORECAST:
      host = "api.open-meteo.com";
      url = "/v1/forecast?latitude=" + String(cfg.lat, 4) +
            "&longitude=" + String(cfg.lon, 4) +
            "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
            "&forecast_days=4&timezone=auto";
      break;
    case TASK_EXTIP:
      host = "ipinfo.io";
      url = "/json";
      break;
  }
  mlog.printf("[NET] start %d -> %s%s\n", t, host.c_str(), url.c_str());
  http.begin(host, url);
}

static void finish_task(NetTask t, const String &raw) {
  if (t == TASK_WEATHER) { parse_weather_body(raw, gWeather); gUpdated = true; }
  else if (t == TASK_FORECAST) { parse_forecast_body(raw, gForecast); gUpdated = true; }
  else if (t == TASK_EXTIP) {
    String ip;
    if (parse_extip_body(raw, ip) && ip.length()) gExtIp = ip;
    gUpdated = true;
  }
}

static void next_task() {
  if (netTask == TASK_WEATHER) { netTask = TASK_FORECAST; start_task(netTask); }
  else if (netTask == TASK_FORECAST) { netTask = TASK_EXTIP; start_task(netTask); }
  else {
    netLastCycle = millis();
    gUpdated = true;
    gFirstDone = true;
    netActive = false;
    mlog.println("[NET] cycle complete");
  }
}

void netfsm_tick() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!netActive) {
    if (netFirst || millis() - netLastCycle >= netInterval) {
      netFirst = false;
      netActive = true;
      netTask = TASK_WEATHER;
      start_task(netTask);
    }
    return;
  }

  http.tick();
  if (http.done()) {
    String raw = http.body();
    http.consume();
    finish_task(netTask, raw);
    next_task();
  } else if (http.failed()) {
    http.consume();
    next_task();   // skip failed task, keep cycle moving
  }
}

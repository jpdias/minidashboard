#pragma once
#include <Arduino.h>

struct Weather {
  float temp = 0.0f;
  int humidity = 0;
  int code = 0;
  char desc[24] = {0};
  bool valid = false;
};

struct DayForecast {
  int code = 0;
  float tmin = 0.0f;
  float tmax = 0.0f;
  bool valid = false;
};

struct Forecast {
  DayForecast days[3];
  bool valid = false;
};

void time_begin();
void time_update();                 // sync NTP + apply TZ
void time_now(int &h, int &m, int &s, int &dow, int &day, int &mon, int &yr);
const char* dow_name(int d);

bool weather_fetch(float lat, float lon, Weather &w);
bool forecast_fetch(float lat, float lon, Forecast &f);
const char* weather_icon(int code);

// Fetches the public (external) IP via ipify. Returns empty string on failure.
String external_ip_fetch();

// Shared body parsers (used by blocking wrappers and the non-blocking FSM)
bool parse_weather_body(const String &body, Weather &w);
bool parse_forecast_body(const String &body, Forecast &f);
bool parse_extip_body(const String &body, String &ip);


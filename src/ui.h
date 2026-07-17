#pragma once
#include <Arduino.h>
#include "nettime.h"
#include "flight.h"

void ui_begin();
void ui_poweroff();
void ui_poweron();
bool ui_is_on();
void ui_screen_loading(unsigned long waitedMs, unsigned long timeoutMs);
void ui_loading_update(unsigned long waitedMs, unsigned long timeoutMs);

void ui_draw_icon(int code, int cx, int cy, uint16_t color);
void ui_screen_tag(int idx, int total);
void ui_draw_wifi_bars(int x, int y);      // cellular-style signal bars
void ui_wifi_indicator(bool connected);   // small offline marker overlay

void ui_draw_clock_static(int h, int m, int dow, int day, int mon, int yr);
void ui_draw_clock(int h, int m, int s, int dow, int day, int mon, int yr);
void ui_draw_seconds(int s);
void ui_draw_uptime(unsigned long uptime);

void ui_screen_clock(int h, int m, int s, int dow, int day, int mon, int yr,
                     const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);
void ui_screen_forecast(int h, int m, int s, const Forecast &f);
void ui_screen_network(int rssi, String intIp, String extIp, unsigned long uptime);
void ui_screen_detail(int h, int m, int s, const Weather &w);
void ui_screen_monitors();
void ui_screen_esphome();
void ui_screen_flight(const FlightData &fd, int rangeNm);
void ui_draw_flightinfo(const FlightData &fd);   // closest-flight box on Clock screen
void ui_screen_system(int rssi, String intIp, unsigned long uptime);
void ui_system_update(int rssi, unsigned long uptime);   // refresh dynamic values

// legacy/combined (still referenced)
void ui_draw(int h, int m, int s, int dow, int day, int mon, int yr,
             const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);
void ui_draw_full(int h, int m, int s, int dow, int day, int mon, int yr,
                   const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);
void ui_draw_weather(const Weather &w);
void ui_draw_metrics(bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);

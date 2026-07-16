#pragma once
#include <Arduino.h>
#include "nettime.h"

void ui_begin();
void ui_poweroff();
void ui_poweron();
bool ui_is_on();

void ui_draw_icon(int code, int cx, int cy, uint16_t color);
void ui_screen_tag(int idx, int total);

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

// legacy/combined (still referenced)
void ui_draw(int h, int m, int s, int dow, int day, int mon, int yr,
             const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);
void ui_draw_full(int h, int m, int s, int dow, int day, int mon, int yr,
                   const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);
void ui_draw_weather(const Weather &w);
void ui_draw_metrics(bool metrics, int rssi, String intIp, String extIp, unsigned long uptime);

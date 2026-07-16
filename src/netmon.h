#pragma once
#include <Arduino.h>
#include "config.h"

struct MonitorState {
  bool online = false;
  int  latency = 0;      // ms, -1 if unreachable
  unsigned long lastSeen = 0;   // millis when last online
  unsigned long downtime = 0;   // accumulated downtime ms
  unsigned long totalSince = 0; // tracking start
};

extern MonitorState monitors[MONITOR_MAX];

void monitors_begin();
// Checks one host per call (round-robin) so it never blocks the loop.
void monitors_tick();
bool monitor_reachable(const char* host, int &ms);
float monitor_uptime_pct(int i);

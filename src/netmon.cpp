#include "netmon.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

MonitorState monitors[MONITOR_MAX];
static int probeIdx = 0;
static unsigned long lastProbe = 0;

void monitors_begin() {
  unsigned long now = millis();
  for (int i = 0; i < MONITOR_MAX; i++) {
    monitors[i].totalSince = now;
    monitors[i].lastSeen = now;
  }
}

bool monitor_reachable(const char* host, int &ms) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  client.setTimeout(400);
  unsigned long t0 = millis();
  bool ok = client.connect(host, 80);
  if (!ok) ok = client.connect(host, 443);
  if (!ok) { ms = -1; return false; }
  // Send a minimal HEAD request
  client.print(String("HEAD / HTTP/1.1\r\nHost: ") + host +
               "\r\nUser-Agent: miniTV\r\nConnection: close\r\n\r\n");
  // Wait for any response byte (bounded so we never stall the loop)
  unsigned long tw = millis();
  while (!client.available() && millis() - tw < 800) yield();
  ms = millis() - t0;
  bool got = client.available() > 0;
  client.stop();
  return got;
}

void monitors_tick() {
  unsigned long now = millis();
  if (now - lastProbe < 1500) return;  // throttle: one probe every 1.5s
  lastProbe = now;

  // skip empty slots
  int tries = 0;
  while (tries < MONITOR_MAX) {
    int i = probeIdx;
    probeIdx = (probeIdx + 1) % MONITOR_MAX;
    tries++;
    if (cfg.monitors[i][0] == 0) continue;

    int ms = 0;
    bool up = monitor_reachable(cfg.monitors[i], ms);
    MonitorState &m = monitors[i];
    if (up) {
      if (!m.online) m.lastSeen = now;
      m.online = true;
      m.latency = ms;
    } else {
      if (m.online) m.downtime += (now - m.lastSeen);
      m.online = false;
      m.latency = -1;
    }
    break;  // one probe per tick
  }
}

float monitor_uptime_pct(int i) {
  MonitorState &m = monitors[i];
  unsigned long total = millis() - m.totalSince;
  if (total == 0) return 100.0f;
  unsigned long up = total - m.downtime;
  if (!m.online) up -= (millis() - m.lastSeen);
  if (up < 0) up = 0;
  return (up * 100.0f) / total;
}

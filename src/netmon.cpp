#include "logbuf.h"
#include "netmon.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

MonitorState monitors[MONITOR_MAX];
static int probeIdx = 0;
static unsigned long lastProbe = 0;

// One host probed per tick, round-robin, with bounded connect timeouts so the
// loop is never stalled for long (WiFiClient::connect is synchronous on ESP8266).
static WiFiClient pbClient;
static int pbHost = -1;
static uint16_t pbPort = 80;

void monitors_begin() {
  unsigned long now = millis();
  for (int i = 0; i < MONITOR_MAX; i++) {
    monitors[i].totalSince = now;
    monitors[i].lastSeen = now;
  }
}

// Legacy blocking probe (kept for compatibility / one-off checks).
bool monitor_reachable(const char* host, int &ms) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  client.setTimeout(400);
  unsigned long t0 = millis();
  bool ok = client.connect(host, 80);
  if (!ok) ok = client.connect(host, 443);
  if (!ok) { ms = -1; return false; }
  ms = millis() - t0;
  client.stop();
  return true;
}

static void probe_result(bool up, int ms) {
  unsigned long now = millis();
  MonitorState &m = monitors[pbHost];
  if (up) {
    if (!m.online) m.lastSeen = now;
    m.online = true;
    m.latency = ms;
  } else {
    if (m.online) m.downtime += (now - m.lastSeen);
    m.online = false;
    m.latency = -1;
  }
  pbClient.stop();
}

void monitors_tick() {
  unsigned long now = millis();
  if (now - lastProbe < 1500) return;   // throttle: one host every 1.5s
  lastProbe = now;
  if (WiFi.status() != WL_CONNECTED) return;

  // Pick next non-empty host round-robin.
  int tries = 0;
  while (tries < MONITOR_MAX) {
    int i = probeIdx;
    probeIdx = (probeIdx + 1) % MONITOR_MAX;
    tries++;
    if (cfg.monitors[i][0] == 0) continue;

    pbHost = i;
    unsigned long t0 = millis();
    pbClient.stop();
    pbClient.setTimeout(600);              // bound the connect() call
    bool ok = pbClient.connect(cfg.monitors[i], 80);
    if (!ok) { pbClient.stop(); ok = pbClient.connect(cfg.monitors[i], 443); }
    probe_result(ok, ok ? (int)(millis() - t0) : -1);
    return;
  }
}

float monitor_uptime_pct(int i) {
  MonitorState &m = monitors[i];
  unsigned long total = millis() - m.totalSince;
  if (total == 0) return 100.0f;
  unsigned long up = total - m.downtime;
  if (!m.online) up -= (millis() - m.lastSeen);
  return (up * 100.0f) / total;
}

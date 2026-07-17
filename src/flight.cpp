#include "logbuf.h"
#include "flight.h"
#include "config.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char* FL_HOST = "opendata.adsb.fi";
static const uint16_t FL_PORT = 443;
static const unsigned long FL_INTERVAL = 5000;   // 5s refresh
static const size_t FL_MAX_BYTES = 12000;        // hard cap on response we buffer

static FlightData gData;
static bool gUpdated = false;

// One BearSSL client, created per-fetch to release its ~16-22KB buffers between
// requests. Non-blocking phases keep the main loop (and clock) alive.
static BearSSL::WiFiClientSecure *cli = nullptr;
static String buf;

enum Phase { P_IDLE, P_CONN, P_WAIT, P_READ };
static Phase phase = P_IDLE;
static unsigned long timer = 0;
static unsigned long lastCycle = 0;
static bool first = true;

const FlightData& flight_data() { return gData; }

bool flight_updated() {
  if (gUpdated) { gUpdated = false; return true; }
  return false;
}

void flight_begin() {
  phase = P_IDLE;
  first = true;
  lastCycle = 0;
  gData.valid = false;
  gData.count = 0;
}

static void cleanup() {
  if (cli) { cli->stop(); delete cli; cli = nullptr; }
  buf = "";
}

static void fail(const char *why) {
  mlog.printf("[FLT] %s\n", why);
  cleanup();
  phase = P_IDLE;
  lastCycle = millis();
}

static int insert_sorted(FlightAc *arr, int n, const FlightAc &a) {
  // Keep the FLIGHT_MAX closest aircraft, sorted ascending by distance.
  if (n < FLIGHT_MAX) {
    int i = n - 1;
    while (i >= 0 && arr[i].dst > a.dst) { arr[i + 1] = arr[i]; i--; }
    arr[i + 1] = a;
    return n + 1;
  }
  if (a.dst >= arr[n - 1].dst) return n;   // farther than our worst, drop
  int i = n - 2;
  while (i >= 0 && arr[i].dst > a.dst) { arr[i + 1] = arr[i]; i--; }
  arr[i + 1] = a;
  return n;
}

static void parse(const String &raw) {
  int he = raw.indexOf("\r\n\r\n");
  String j = (he >= 0) ? raw.substring(he + 4) : raw;
  int b = j.indexOf('{');
  if (b > 0) j = j.substring(b);

  StaticJsonDocument<160> filter;
  JsonObject fac = filter["aircraft"].createNestedObject();
  fac["flight"] = true;
  fac["alt_baro"] = true;
  fac["track"] = true;
  fac["dst"] = true;
  fac["dir"] = true;

  DynamicJsonDocument doc(6144);
  DeserializationError err =
      deserializeJson(doc, j.c_str(), DeserializationOption::Filter(filter));
  if (err) { mlog.printf("[FLT] parse err %s\n", err.c_str()); gData.valid = false; return; }

  JsonArray arr = doc["aircraft"];
  int n = 0, total = 0;
  for (JsonObject o : arr) {
    total++;
    FlightAc a;
    const char *fl = o["flight"] | "";
    strncpy(a.flight, fl, sizeof(a.flight) - 1);
    a.flight[sizeof(a.flight) - 1] = 0;
    for (int k = (int)strlen(a.flight) - 1; k >= 0 && a.flight[k] == ' '; k--) a.flight[k] = 0;
    a.dst = o["dst"] | 999.0f;
    a.dir = o["dir"] | 0.0f;
    a.track = o["track"] | -1.0f;
    a.alt = o["alt_baro"] | 0;
    n = insert_sorted(gData.ac, n, a);
  }
  gData.count = n;
  gData.total = total;
  gData.valid = true;
  gUpdated = true;
  mlog.printf("[FLT] %d aircraft (showing %d)\n", total, n);
}

void flight_tick() {
  if (cfg.flight_range <= 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  switch (phase) {
    case P_IDLE:
      if (first || millis() - lastCycle >= FL_INTERVAL) {
        first = false;
        cli = new BearSSL::WiFiClientSecure();
        if (!cli) { fail("alloc fail"); return; }
        cli->setInsecure();
        cli->setBufferSizes(4096, 512);   // smaller TLS buffers to fit heap
        buf = "";
        phase = P_CONN;
        timer = millis();
      }
      break;

    case P_CONN:
      if (cli->connect(FL_HOST, FL_PORT)) {
        String url = String("/api/v2/lat/") + String(cfg.lat, 4) +
                     "/lon/" + String(cfg.lon, 4) +
                     "/dist/" + String(cfg.flight_range);
        cli->print(String("GET ") + url + " HTTP/1.1\r\n" +
                   "Host: " + FL_HOST + "\r\n" +
                   "User-Agent: miniDash\r\n" +
                   "Connection: close\r\n\r\n");
        phase = P_WAIT;
        timer = millis();
      } else if (millis() - timer > 8000) {
        fail("connect fail");
      }
      break;

    case P_WAIT:
      if (cli->available()) { phase = P_READ; timer = millis(); }
      else if (millis() - timer > 6000) fail("wait timeout");
      break;

    case P_READ:
      while (cli->available() && buf.length() < FL_MAX_BYTES) buf += (char)cli->read();
      if (buf.length() >= FL_MAX_BYTES) {
        // Truncated: still try to parse what we have (JSON may be incomplete).
        parse(buf);
        cleanup();
        phase = P_IDLE;
        lastCycle = millis();
      } else if (!cli->connected() && !cli->available()) {
        parse(buf);
        cleanup();
        phase = P_IDLE;
        lastCycle = millis();
      } else if (millis() - timer > 8000) {
        fail("read stall");
      }
      break;
  }
}

#include "moon.h"
#include "logbuf.h"
#include "config.h"
#include "nettime.h"
#include "tlslock.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

static const char*  MOON_HOST = "aa.usno.navy.mil";
static const uint16_t MOON_PORT = 443;
static const uint32_t MOON_MIN_HEAP = 22000;   // need contiguous heap for TLS

static MoonInfo gMoon;
static bool gUpdated = false;
static int  gFetchedYday = -1;    // local day-of-year we last fetched for

enum MPhase { M_IDLE, M_CONN, M_WAIT, M_HDR, M_READ };
static MPhase phase = M_IDLE;
static BearSSL::WiFiClientSecure *cli = nullptr;
static unsigned long timer = 0;
static unsigned long lastAttempt = 0;

void moon_begin() {
  gMoon.valid = false;
  gFetchedYday = -1;
}

const MoonInfo& moon_data() { return gMoon; }

bool moon_updated() {
  if (gUpdated) { gUpdated = false; return true; }
  return false;
}

static void cleanup() {
  if (cli) { cli->stop(); delete cli; cli = nullptr; }
  tls_release();
}

static void fail(const char* why) {
  mlog.printf("[MOON] fail: %s\n", why);
  cleanup();
  phase = M_IDLE;
  lastAttempt = millis();
}

// Extract "HH:MM" from a USNO time string ("06:12" already, keep first 5 chars).
static void copy_hm(const char* src, char* dst) {
  dst[0] = 0;
  if (!src || strlen(src) < 5) return;
  strncpy(dst, src, 5);
  dst[5] = 0;
}

// Map a USNO "curphase" name to a rough phase fraction for the drawn glyph.
static float phase_fraction(const char* name, int illum) {
  if (!name) name = "";
  if (strstr(name, "New")) return 0.0f;
  if (strstr(name, "First Quarter")) return 0.25f;
  if (strstr(name, "Full")) return 0.5f;
  if (strstr(name, "Last Quarter")) return 0.75f;
  // Interpolate from illumination; waning names put us on the second half.
  float f = illum / 100.0f;                 // 0..1 illuminated
  bool waning = strstr(name, "Waning") || strstr(name, "Last");
  // illum -> distance from full; convert to phase 0..1
  float p = acosf(1.0f - 2.0f * f) / (2.0f * (float)M_PI); // 0..0.5
  return waning ? (1.0f - p) : p;
}

bool parse_moon_body(const String &body, MoonInfo &out) {
  int brace = body.indexOf('{');
  if (brace < 0) { mlog.println("[MOON] no JSON"); return false; }
  // The response is small (~1.3 KB), so parse it whole without a filter.
  DynamicJsonDocument doc(3072);
  DeserializationError err = deserializeJson(doc, body.c_str() + brace);
  if (err) { mlog.printf("[MOON] parse err: %s\n", err.c_str()); return false; }

  JsonObject d = doc["properties"]["data"];
  if (d.isNull()) { mlog.println("[MOON] no data"); return false; }

  out.sunrise[0] = out.sunset[0] = out.moonrise[0] = out.moonset[0] = 0;
  for (JsonObject e : d["sundata"].as<JsonArray>()) {
    const char* p = e["phen"] | "";
    if (!strcmp(p, "Rise")) copy_hm(e["time"] | "", out.sunrise);
    else if (!strcmp(p, "Set")) copy_hm(e["time"] | "", out.sunset);
  }
  for (JsonObject e : d["moondata"].as<JsonArray>()) {
    const char* p = e["phen"] | "";
    if (!strcmp(p, "Rise")) copy_hm(e["time"] | "", out.moonrise);
    else if (!strcmp(p, "Set")) copy_hm(e["time"] | "", out.moonset);
  }

  const char* fi = d["fracillum"] | "";      // e.g. "63%"
  out.illum = atoi(fi);
  const char* cp = d["curphase"] | "";
  strncpy(out.name, cp, sizeof(out.name) - 1);
  out.name[sizeof(out.name) - 1] = 0;
  out.phase = phase_fraction(cp, out.illum);
  out.valid = true;
  mlog.printf("[MOON] OK sun %s/%s moon %s/%s illum %d%% %s\n",
              out.sunrise, out.sunset, out.moonrise, out.moonset, out.illum, out.name);
  return true;
}

// Read whatever bytes are currently available (non-blocking) into gBody, tracking
// header/body split. Returns true when the response is complete (socket closed).
static String gBody;
static bool gInBody = false;
static String gHdrLine;

static bool read_chunk(WiFiClient &c) {
  // Drain only what's buffered right now; do NOT spin waiting for more.
  int budget = 512;                       // cap per tick to keep the loop snappy
  while (c.available() && budget-- > 0) {
    char ch = c.read();
    if (!gInBody) {
      gHdrLine += ch;
      if (ch == '\n') {
        if (gHdrLine == "\r\n" || gHdrLine == "\n") gInBody = true;
        gHdrLine = "";
      }
    } else {
      gBody += ch;
      if (gBody.length() > 6000) return true;   // safety cap -> treat as done
    }
  }
  // Done when the peer closed and nothing is left to read.
  return (!c.connected() && !c.available());
}

void moon_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!time_is_synced()) return;
  // Small boot delay so the first screen renders before the (blocking) handshake.
  if (millis() < 8000UL) return;

  switch (phase) {
    case M_IDLE: {
      // Day-of-year key so we fetch exactly once per local calendar day.
      time_t now = time(nullptr);
      struct tm *lt = localtime(&now);
      int yday = lt->tm_yday;
      if (yday == gFetchedYday) return;                 // already have today
      if (millis() - lastAttempt < 10000 && lastAttempt) return;  // retry backoff
      if (ESP.getMaxFreeBlockSize() < MOON_MIN_HEAP) {
        lastAttempt = millis();
        return;
      }
      if (!tls_try_acquire()) return;   // flight fetch busy; retry next loop
      cli = new BearSSL::WiFiClientSecure();
      if (!cli) { fail("alloc"); return; }
      cli->setInsecure();
      cli->setBufferSizes(4096, 512);
      cli->setTimeout(3000);
      phase = M_CONN;
      timer = millis();
      break;
    }

    case M_CONN:
      if (cli->connect(MOON_HOST, MOON_PORT)) {
        int h, m, s, dow, day, mon, yr;
        time_now(h, m, s, dow, day, mon, yr);
        long off = time_tz_offset();
        // tz in hours (float ok); USNO uses east-positive, same as our offset.
        char tzbuf[12];
        snprintf(tzbuf, sizeof(tzbuf), "%.2f", off / 3600.0);
        String url = String("/api/rstt/oneday?date=") + yr + "-" + mon + "-" + day +
                     "&coords=" + String(cfg.lat, 4) + "," + String(cfg.lon, 4) +
                     "&tz=" + tzbuf;
        cli->print(String("GET ") + url + " HTTP/1.1\r\n" +
                   "Host: " + MOON_HOST + "\r\n" +
                   "User-Agent: miniDash\r\n" +
                   "Connection: close\r\n\r\n");
        mlog.printf("[MOON] GET %s\n", url.c_str());
        phase = M_WAIT;
        timer = millis();
      } else if (millis() - timer > 8000) {
        fail("connect");
      }
      break;

    case M_WAIT:
      if (cli->available()) {
        gBody = ""; gHdrLine = ""; gInBody = false;
        phase = M_READ; timer = millis();
      } else if (!cli->connected()) fail("closed early");
      else if (millis() - timer > 8000) fail("wait timeout");
      break;

    case M_HDR:   // unused; kept for enum symmetry
      phase = M_READ;
      break;

    case M_READ: {
      bool done = read_chunk(*cli);
      if (!done && millis() - timer < 8000) break;   // keep reading next ticks
      cleanup();
      mlog.printf("[MOON] body len=%d\n", gBody.length());
      if (parse_moon_body(gBody, gMoon)) {
        time_t now = time(nullptr);
        gFetchedYday = localtime(&now)->tm_yday;
        gUpdated = true;
      } else {
        lastAttempt = millis();
      }
      gBody = "";
      phase = M_IDLE;
      break;
    }
  }
}

// Synchronous, deterministic fetch used at boot (and any caller that can block).
// Blocks until the full response is read and parsed, or until timeoutMs elapses.
// No FSM interleaving, no contention with the flight fetcher (it isn't running yet
// at boot), so this is the reliable path that avoids the "moon sometimes doesn't
// load" race. On success sets gFetchedYday so moon_tick() won't re-fetch today.
bool moon_fetch_blocking(unsigned long timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) { mlog.println("[MOON] block: no wifi"); return false; }
  if (!time_is_synced()) { mlog.println("[MOON] block: clock not synced"); return false; }
  if (!tls_try_acquire()) { mlog.println("[MOON] block: tls busy"); return false; }

  BearSSL::WiFiClientSecure *c = new BearSSL::WiFiClientSecure();
  if (!c) { tls_release(); mlog.println("[MOON] block: alloc fail"); return false; }
  c->setInsecure();
  c->setBufferSizes(4096, 512);
  c->setTimeout(3000);

  int h, m, s, dow, day, mon, yr;
  time_now(h, m, s, dow, day, mon, yr);
  long off = time_tz_offset();
  char tzbuf[12];
  snprintf(tzbuf, sizeof(tzbuf), "%.2f", off / 3600.0);
  String url = String("/api/rstt/oneday?date=") + yr + "-" + mon + "-" + day +
               "&coords=" + String(cfg.lat, 4) + "," + String(cfg.lon, 4) +
               "&tz=" + tzbuf;
  String host = MOON_HOST;

  bool ok = false;
  if (c->connect(host.c_str(), MOON_PORT)) {
    c->print(String("GET ") + url + " HTTP/1.1\r\n" +
             "Host: " + host + "\r\n" +
             "User-Agent: miniDash\r\n" +
             "Connection: close\r\n\r\n");
    String body; bool inBody = false; String hdr;
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
      ESP.wdtFeed();
      while (c->available()) {
        char ch = c->read();
        if (!inBody) {
          hdr += ch;
          if (ch == '\n' && (hdr == "\r\n" || hdr == "\n")) inBody = true;
          hdr = (ch == '\n') ? "" : hdr;
        } else {
          body += ch;
        }
      }
      if (inBody && !c->connected() && !c->available()) break;
      if (body.length() > 6000) break;
    }
    mlog.printf("[MOON] block body len=%d\n", body.length());
    if (parse_moon_body(body, gMoon)) {
      time_t now = time(nullptr);
      gFetchedYday = localtime(&now)->tm_yday;
      gUpdated = true;
      ok = true;
    } else {
      lastAttempt = millis();
    }
  } else {
    mlog.println("[MOON] block: connect failed");
  }
  c->stop(); delete c;
  tls_release();
  return ok;
}

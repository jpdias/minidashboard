#include "moon.h"
#include "logbuf.h"
#include "config.h"
#include "nettime.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

// Sun + Moon data from sunrise-sunset.org v2 — plain HTTP (no TLS, no API key).
// This endpoint explicitly supports non-TLS requests for ESP8266/Arduino and
// returns sun + moon in one fast call (ISO 8601 local times). It is much faster
// than the old USNO TLS endpoint and needs no BearSSL session, so it does not
// contend with the flight radar's TLS client.
static const char*  MOON_HOST = "api.sunrise-sunset.org";
static const uint16_t MOON_PORT = 80;

static MoonInfo gMoon;
static bool gUpdated = false;
static int  gFetchedYday = -1;    // local day-of-year we last fetched for
static int  gFetchYr = 0, gFetchMon = 0, gFetchDay = 0;  // date requested by FSM

enum MPhase { M_IDLE, M_CONN, M_WAIT, M_READ };
static MPhase phase = M_IDLE;
static WiFiClient *cli = nullptr;
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
}

static void fail(const char* why) {
  mlog.printf("[MOON] fail: %s\n", why);
  cleanup();
  phase = M_IDLE;
  lastAttempt = millis();
}

// Slice "HH:MM" from an ISO 8601 local time ("2026-07-18T06:15:03+01:00").
// Null/missing input (ArduinoJson yields "") yields an empty string.
static void to_iso_hhmm(const char* src, char* dst) {
  dst[0] = 0;
  if (!src || strlen(src) < 16) return;   // need through "...THH:MM"
  snprintf(dst, 6, "%.2s:%.2s", src + 11, src + 14);
}

// Build the request URL for the local date. The v2 endpoint returns times in the
// location's own local timezone, so no tz parameter is needed.
static String build_url(int yr, int mon, int day) {
  char date[12];
  snprintf(date, sizeof(date), "%04d-%02d-%02d", yr, mon, day);
  return String("/v2?lat=") + String(cfg.lat, 4) +
         "&lng=" + String(cfg.lon, 4) +
         "&date=" + date;
}

// Map a v2 moon_phase name to a rough 0..1 fraction for the glyph (no numeric
// phase is provided by this API, unlike the .io one).
static float phase_fraction(const char* name, int illum) {
  if (!name) name = "";
  if (strstr(name, "New")) return 0.0f;
  if (strstr(name, "First Quarter")) return 0.25f;
  if (strstr(name, "Full")) return 0.5f;
  if (strstr(name, "Last Quarter")) return 0.75f;
  // Fall back to illumination: illum -> distance from new, waning on second half.
  float f = constrain(illum, 0, 100) / 100.0f;
  bool waning = strstr(name, "Waning") || strstr(name, "Last");
  float p = acosf(constrain(1.0f - 2.0f * f, -1.0f, 1.0f)) / (2.0f * (float)M_PI);
  return waning ? (1.0f - p) : p;
}

bool parse_moon_body(const String &body, MoonInfo &out) {
  int brace = body.indexOf('{');
  if (brace < 0) { mlog.println("[MOON] no JSON"); return false; }
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, body.c_str() + brace);
  if (err) { mlog.printf("[MOON] parse err: %s\n", err.c_str()); return false; }

  // v2 returns top-level fields. moonrise/moonset are null when they don't occur.
  out.sunrise[0] = out.sunset[0] = out.moonrise[0] = out.moonset[0] = 0;
  to_iso_hhmm(doc["sunrise"] | "", out.sunrise);
  to_iso_hhmm(doc["sunset"]  | "", out.sunset);
  to_iso_hhmm(doc["moonrise"] | "", out.moonrise);
  to_iso_hhmm(doc["moonset"]  | "", out.moonset);   // null -> empty

  out.illum = (int)roundf(doc["moon_illumination"] | 0.0f);
  const char* cp = doc["moon_phase"] | "";
  strncpy(out.name, cp, sizeof(out.name) - 1);
  out.name[sizeof(out.name) - 1] = 0;
  out.phase = phase_fraction(cp, out.illum);

  out.valid = true;
  mlog.printf("[MOON] OK sun %s/%s moon %s/%s illum %d%% %s\n",
              out.sunrise, out.sunset, out.moonrise, out.moonset, out.illum, out.name);
  return true;
}

// Read whatever bytes are currently available into gBody, tracking the header/body
// split. Returns true when the response is complete (socket closed).
static String gBody;
static bool gInBody = false;
static String gHdrLine;

static bool read_chunk(WiFiClient &c) {
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
      if (gBody.length() > 4000) return true;   // safety cap -> treat as done
    }
  }
  return (!c.connected() && !c.available());
}

void moon_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!time_is_synced()) return;

  switch (phase) {
    case M_IDLE: {
      // Day-of-year key so we fetch exactly once per local calendar day.
      time_t now = time(nullptr);
      const struct tm *lt = localtime(&now);
      int yday = lt->tm_yday;
      if (yday == gFetchedYday) return;                 // already have today
      if (millis() - lastAttempt < 10000 && lastAttempt) return;  // retry backoff
      int h, m, s, dow, day, mon, yr;
      time_now(h, m, s, dow, day, mon, yr);
      cli = new WiFiClient();
      if (!cli) { fail("alloc"); return; }
      cli->setTimeout(3000);
      // Stash the date we're asking for so M_READ can stamp gFetchedYday.
      gFetchYr = yr; gFetchMon = mon; gFetchDay = day;
      phase = M_CONN;
      timer = millis();
      break;
    }

    case M_CONN: {
      String url = build_url(gFetchYr, gFetchMon, gFetchDay);
      if (cli->connect(MOON_HOST, MOON_PORT)) {
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
    }

    case M_WAIT:
      if (cli->available()) {
        gBody = ""; gHdrLine = ""; gInBody = false;
        phase = M_READ; timer = millis();
      } else if (!cli->connected()) fail("closed early");
      else if (millis() - timer > 8000) fail("wait timeout");
      break;

    case M_READ: {
      bool done = read_chunk(*cli);
      if (!done && millis() - timer < 8000) break;   // keep reading next ticks
      cleanup();
      mlog.printf("[MOON] body len=%d\n", gBody.length());
      if (parse_moon_body(gBody, gMoon)) {
        // Mark today fetched using the date we actually requested.
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_year = gFetchYr - 1900; t.tm_mon = gFetchMon - 1; t.tm_mday = gFetchDay;
        t.tm_isdst = -1;
        time_t tt = mktime(&t);
        gFetchedYday = (tt >= 0) ? localtime(&tt)->tm_yday : -1;
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
// Plain HTTP, so it's fast and never contends with the flight radar's TLS client.
// On success sets gFetchedYday so moon_tick() won't re-fetch the same local day.
bool moon_fetch_blocking(unsigned long timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) { mlog.println("[MOON] block: no wifi"); return false; }
  if (!time_is_synced()) { mlog.println("[MOON] block: clock not synced"); return false; }

  WiFiClient c;
  c.setTimeout(3000);
  int h, m, s, dow, day, mon, yr;
  time_now(h, m, s, dow, day, mon, yr);
  String url = build_url(yr, mon, day);

  bool ok = false;
  if (c.connect(MOON_HOST, MOON_PORT)) {
    c.print(String("GET ") + url + " HTTP/1.1\r\n" +
            "Host: " + MOON_HOST + "\r\n" +
            "User-Agent: miniDash\r\n" +
            "Connection: close\r\n\r\n");
    String body; bool inBody = false; String hdr;
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
      ESP.wdtFeed();
      while (c.available()) {
        char ch = c.read();
        if (!inBody) {
          hdr += ch;
          if (ch == '\n' && (hdr == "\r\n" || hdr == "\n")) inBody = true;
          hdr = (ch == '\n') ? "" : hdr;
        } else {
          body += ch;
        }
      }
      if (inBody && !c.connected() && !c.available()) break;
      if (body.length() > 4000) break;
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
  c.stop();
  return ok;
}

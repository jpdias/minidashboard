#include "moon.h"
#include "logbuf.h"
#include "config.h"
#include "nettime.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <time.h>

// Sun + Moon data from sunrisesunset.io — plain HTTP (no TLS, no API key). This is
// dramatically faster than the old USNO TLS endpoint and needs no heap for a BearSSL
// session, so it no longer contends with the flight radar's TLS client.
static const char*  MOON_HOST = "api.sunrisesunset.io";
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

// Convert a sunrisesunset.io 12-hour time ("7:12:01 AM" / "12:10:59 AM") to our
// 24-hour "HH:MM" string. Missing/garbage input yields an empty string.
static void to_hhmm(const char* src, char* dst) {
  dst[0] = 0;
  if (!src || !*src) return;
  int h = 0, m = 0;
  char ap[3] = {0};
  // Parse "H[:MM[:SS]] AM/PM" — sscanf with %d handles 1- or 2-digit hours.
  if (sscanf(src, "%d:%d:%*d %2s", &h, &m, ap) < 2 &&
      sscanf(src, "%d:%d %2s", &h, &m, ap) < 2) return;
  bool pm = (ap[0] == 'P' || ap[0] == 'p');
  if (pm && h != 12) h += 12;
  if (!pm && h == 12) h = 0;          // 12 AM -> 00
  if (h < 0 || h > 23 || m < 0 || m > 59) return;
  snprintf(dst, 6, "%02d:%02d", h, m);
}

// Build the request URL for the local date. sunrisesunset.io returns times already
// in the location's local timezone, so no tz parameter is needed.
static String build_url(int yr, int mon, int day) {
  char date[12];
  snprintf(date, sizeof(date), "%04d-%02d-%02d", yr, mon, day);
  return String("/json?lat=") + String(cfg.lat, 4) +
         "&lng=" + String(cfg.lon, 4) +
         "&date=" + date;
}

bool parse_moon_body(const String &body, MoonInfo &out) {
  int brace = body.indexOf('{');
  if (brace < 0) { mlog.println("[MOON] no JSON"); return false; }
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, body.c_str() + brace);
  if (err) { mlog.printf("[MOON] parse err: %s\n", err.c_str()); return false; }

  JsonObject r = doc["results"];
  if (r.isNull()) { mlog.println("[MOON] no results"); return false; }

  out.sunrise[0] = out.sunset[0] = out.moonrise[0] = out.moonset[0] = 0;
  to_hhmm(r["sunrise"] | "", out.sunrise);
  to_hhmm(r["sunset"]  | "", out.sunset);
  to_hhmm(r["moonrise"] | "", out.moonrise);
  to_hhmm(r["moonset"]  | "", out.moonset);

  out.illum = (int)roundf(r["moon_illumination"] | 0.0f);
  const char* cp = r["moon_phase"] | "";
  strncpy(out.name, cp, sizeof(out.name) - 1);
  out.name[sizeof(out.name) - 1] = 0;
  // moon_phase_value is a ready-made 0..1 fraction for the glyph; fall back to 0.5.
  out.phase = r["moon_phase_value"] | 0.5f;

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
      struct tm *lt = localtime(&now);
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

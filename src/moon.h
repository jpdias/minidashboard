#pragma once
#include <Arduino.h>

// Sun + Moon rise/set and moon phase for the current local day, fetched once per
// day from the USNO Astronomical Applications API (rstt/oneday). Also computes an
// approximate phase fraction (0..1) from the illumination for drawing a glyph.
struct MoonInfo {
  char  sunrise[6] = {0};    // "HH:MM" local, empty if none
  char  sunset[6] = {0};
  char  moonrise[6] = {0};
  char  moonset[6] = {0};
  int   illum = 0;           // fraction of moon illuminated, percent 0..100
  float phase = 0.0f;        // 0/1 = new, 0.5 = full (derived, for the glyph)
  char  name[20] = {0};      // phase / current-phase text
  bool  valid = false;
};

void moon_begin();
void moon_tick();                  // non-blocking; fetches once per local day
bool moon_updated();               // true once after a successful fetch
const MoonInfo& moon_data();

// Synchronous, deterministic fetch used at boot (and anything that can afford to
// block). Blocks until the response is read and parsed or it times out. Returns
// true on success. This is the reliable path — no FSM interleaving, no contention
// with the flight fetcher. Sets gFetchedYday on success so moon_tick() won't
// re-fetch the same local day.
bool moon_fetch_blocking(unsigned long timeoutMs = 12000);

// Parse a USNO rstt/oneday GeoJSON body into out. Returns true on success.
bool parse_moon_body(const String &body, MoonInfo &out);

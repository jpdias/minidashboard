#pragma once
#include <Arduino.h>

#define FLIGHT_MAX 12

struct FlightAc {
  char flight[10];   // callsign
  float dst;         // distance from center (nm)
  float dir;         // bearing from center (deg, 0=N)
  float track;       // aircraft heading (deg)
  int   alt;         // barometric altitude (ft)
};

struct FlightData {
  FlightAc ac[FLIGHT_MAX];
  int count = 0;      // number of aircraft stored
  int total = 0;      // total reported by API (may exceed count)
  bool valid = false;
};

// Non-blocking flight radar fetcher (TLS to adsb.fi). Refreshes every ~5s.
void flight_begin();
void flight_tick();
bool flight_updated();        // true once after a refresh
const FlightData& flight_data();

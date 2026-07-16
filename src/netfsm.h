#pragma once
#include <Arduino.h>
#include "nettime.h"

// Non-blocking network fetcher (state machine). Drives weather + forecast +
// external-IP fetches one at a time without stalling the main loop.
void netfsm_begin(unsigned long intervalMs);
void netfsm_tick();
bool netfsm_updated();          // true once after a full cycle completes
bool netfsm_first_done();       // true after the first full boot cycle completes

Weather& net_weather();
Forecast& net_forecast();
String net_extip();

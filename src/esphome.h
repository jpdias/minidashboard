#pragma once
#include <Arduino.h>
#include "config.h"

#define EH_MAX 4

enum EhState { EH_IDLE, EH_CONN, EH_WAIT, EH_READ, EH_NEXT };

struct EspHomeState {
  char name[24] = {0};
  char state[16] = {0};
  char uom[8] = {0};
  bool valid = false;
};

void esphome_begin();
void esphome_tick();
const EspHomeState& esphome_state(int i);
int esphome_count();

// Maps unsupported Unicode glyphs (°, µ, ³, ², en/em dash, …) to safe ASCII.
void sanitize_ascii(char *s);

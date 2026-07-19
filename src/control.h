#pragma once
#include <Arduino.h>

// Runtime control of the display, shared between the physical button, the night
// scheduler, and the web/REST API. All functions are safe to call from the main
// loop context (the web server runs cooperatively in loop()).

// Turn the whole display (panel + backlight) on or off.
void control_display_set(bool on);
// Flip the current on/off state. Returns the new state.
bool control_display_toggle();
// True if the display is currently on.
bool control_display_is_on();

// Advance to the next / previous enabled screen (skips disabled ones and the
// flight radar when its range is 0). Wakes the display if it was off.
void control_screen_next();
void control_screen_prev();
// Jump to a specific screen index (0-based). Clamped/ignored if out of range or
// the target screen is disabled. Wakes the display if it was off.
void control_screen_set(int idx);

// Current screen index (0-based) and total screen count.
int control_screen_get();
int control_screen_count();
// Human-readable name for a screen index (or "" if out of range).
const char* control_screen_name(int idx);
// True if the given screen index is currently enabled/selectable.
bool control_screen_enabled(int idx);

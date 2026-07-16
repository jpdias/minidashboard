#pragma once
#include <Arduino.h>
#include <WiFiManager.h>
#include "config.h"

// Starts WiFi (autoConnect) or spawns AP "miniTV-Setup" for first config.
// Adds custom fields to the portal for location / timezone / display.
void portal_begin();

// Call after setup to let the user re-trigger the portal via a flag or button.
void portal_handle();

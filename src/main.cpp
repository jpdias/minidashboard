#include "logbuf.h"
#include <Arduino.h>
#include "config.h"
#include "portal.h"
#include "nettime.h"
#include "netfsm.h"
#include "esphome.h"
#include "ui.h"
#include "netmon.h"

#define BTN_PIN D3

volatile bool btnToggle = false;
bool screenOn = true;
int lastAutoHour = -1;
bool drawnStatic = false;
int screenIndex = 0;          // 0..5 -> screens 1..6
const int SCREEN_COUNT = 6;

void IRAM_ATTR btn_isr() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last > 300) {  // debounce
    btnToggle = true;
    last = now;
  }
}

void setup() {
  Serial.begin(115200);
  mlog.println("\nBooting miniDash...");

  // Watchdog: HW watchdog auto-resets the chip if the loop freezes and stops
  // feeding it. Enable the software WDT with an explicit timeout as well.
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);   // ~8s; must call ESP.wdtFeed() regularly
  mlog.println("[WDT] watchdog enabled (8s)");

  config_load();
  portal_begin();          // connect wifi or spawn AP

  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), btn_isr, FALLING);

  ui_begin();
  time_begin();
  time_update();           // NTP sync + TZ

  netfsm_begin((unsigned long)cfg.weather_interval * 1000UL);
  esphome_begin();
  monitors_begin();

  // --- Boot loader: block on a loading screen until weather (+IP) is fetched ---
  const unsigned long BOOT_TIMEOUT = 20000;
  unsigned long bootStart = millis();
  unsigned long lastLoad = 0;
  mlog.println("[BOOT] waiting for first data fetch...");
  ui_screen_loading(0, BOOT_TIMEOUT);   // draw static frame once
  while (!net_weather().valid && millis() - bootStart < BOOT_TIMEOUT) {
    ESP.wdtFeed();
    portal_handle();
    netfsm_tick();
    esphome_tick();
    if (millis() - lastLoad > 500) {
      lastLoad = millis();
      ui_loading_update(millis() - bootStart, BOOT_TIMEOUT);
    }
  }
  if (net_weather().valid) mlog.println("[BOOT] weather ready");
  else mlog.println("[BOOT] timeout, proceeding");
  delay(400);
}

void draw_screen(int h, int m, int s, int dow, int day, int mon, int yr) {
  Weather &weather = net_weather();
  Forecast &forecast = net_forecast();
  String extIp = net_extip();
  switch (screenIndex) {
    case 0:
      ui_screen_clock(h, m, s, dow, day, mon, yr, weather, cfg.show_metrics,
                     WiFi.RSSI(), WiFi.localIP().toString(), extIp, millis());
      break;
    case 1:
      ui_screen_esphome();
      break;
    case 2:
      ui_screen_forecast(h, m, s, forecast);
      break;
    case 3:
      ui_screen_network(WiFi.RSSI(), WiFi.localIP().toString(), extIp, millis());
      break;
    case 4:
      ui_screen_detail(h, m, s, weather);
      break;
    case 5:
      ui_screen_monitors();
      break;
  }
}

void loop() {
  ESP.wdtFeed();           // keep the watchdog happy; a freeze stops this -> reset
  portal_handle();
  netfsm_tick();
  esphome_tick();
  monitors_tick();

  // --- Button cycles screens ---
  if (btnToggle) {
    btnToggle = false;
    screenIndex = (screenIndex + 1) % SCREEN_COUNT;
    drawnStatic = false;
    mlog.printf("[BTN] screen %d/%d\n", screenIndex + 1, SCREEN_COUNT);
  }

  int h, m, s, dow, day, mon, yr;
  time_now(h, m, s, dow, day, mon, yr);

  // --- Auto night sleep (23:00 - 07:00) ---
  if (h != lastAutoHour) {
    lastAutoHour = h;
    bool night = (h >= 23 || h < 7);
    if (night && screenOn) { screenOn = false; ui_poweroff(); mlog.println("[AUTO] night -> screen OFF"); }
    else if (!night && !screenOn) { screenOn = true; ui_poweron(); drawnStatic = false; mlog.println("[AUTO] day -> screen ON"); }
  }

  // Non-blocking network updates (driven by netfsm_tick)
  bool dataUpdated = netfsm_updated();

  if (screenOn && ui_is_on()) {
    static int lastMin = -1;
    static unsigned long lastSec = 0;

    // Full screen redraw only on: switch, minute change, or new data.
    bool needRedraw = !drawnStatic;
    if (screenIndex == 0 && m != lastMin) needRedraw = true;
    if (dataUpdated) needRedraw = true;

    if (needRedraw) {
      draw_screen(h, m, s, dow, day, mon, yr);
      lastMin = m;
      drawnStatic = true;
    } else if (screenIndex == 0) {
      // Seconds + uptime tick every second in their own boxes (flicker OK)
      if (millis() - lastSec >= 1000) {
        lastSec = millis();
        ui_draw_seconds(s);
        ui_draw_uptime(millis());
      }
    }
  }

  delay(200);
}


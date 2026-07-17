#include "logbuf.h"
#include <Arduino.h>
#include "config.h"
#include "portal.h"
#include "nettime.h"
#include "netfsm.h"
#include "esphome.h"
#include "ui.h"
#include "netmon.h"
#include "flight.h"

#define BTN_PIN D3
#define BL_PIN  D6            // backlight transistor gate/base (see README)

#define BTN_LONG_MS 600       // press >= this = long press (display on/off)

volatile bool btnToggle = false;
bool screenOn = true;
int lastAutoHour = -1;
bool drawnStatic = false;
int screenIndex = 0;          // 0..N -> screens 1..N+1
const int SCREEN_COUNT = 7;   // 5 = flight radar, 6 = system info

// Display off sources: manualOff (long press) and nightOff (schedule). Either
// one turns the whole display off (screen contents + backlight). A day/night
// transition clears the manual override so automatic control resumes.
bool manualOff = false;
bool nightOff = false;

// Drive the backlight pin for the given state, honoring polarity. No-op unless
// backlight control is enabled in config.
static void backlight_write(bool on) {
  if (!cfg.backlight_control) return;
  digitalWrite(BL_PIN, (on == cfg.backlight_active_high) ? HIGH : LOW);
}

// Turn the whole display (screen + backlight) on or off based on the current
// override state. Forces a redraw when turning back on.
static void display_refresh() {
  bool on = !manualOff && !nightOff;
  if (on && !screenOn) {
    screenOn = true;
    ui_poweron();
    drawnStatic = false;
    backlight_write(true);
    mlog.println("[DISP] ON");
  } else if (!on && screenOn) {
    screenOn = false;
    ui_poweroff();
    backlight_write(false);
    mlog.println("[DISP] OFF");
  } else {
    backlight_write(on);   // keep backlight in sync even if screen state matched
  }
}

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

  // Backlight transistor control (optional). Default ON at boot so the panel
  // is never dark during startup even before config takes effect.
  if (cfg.backlight_control) {
    pinMode(BL_PIN, OUTPUT);
    backlight_write(true);
  }

  ui_begin();
  time_begin();
  time_update();           // NTP sync + TZ

  netfsm_begin((unsigned long)cfg.weather_interval * 1000UL);
  esphome_begin();
  monitors_begin();
  flight_begin();

  // Land on the first enabled screen (fall back to Clock if none enabled).
  screenIndex = 0;
  for (int i = 0; i < SCREEN_COUNT; i++) {
    bool disabled = !cfg.screen_enabled[i] || (i == 5 && cfg.flight_range <= 0);
    if (!disabled) { screenIndex = i; break; }
  }

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
      ui_screen_detail(h, m, s, weather);
      break;
    case 4:
      ui_screen_monitors();
      break;
    case 5:
      ui_screen_flight(flight_data(), cfg.flight_range);
      break;
    case 6:
      ui_screen_system(WiFi.RSSI(), WiFi.localIP().toString(), millis());
      break;
  }
}

void wifi_supervise() {
  static bool wasConnected = true;
  static unsigned long lastAttempt = 0;
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    if (!wasConnected) { mlog.println("[WIFI] reconnected"); drawnStatic = false; }
    wasConnected = true;
    return;
  }
  if (wasConnected) {
    mlog.println("[WIFI] connection lost");
    wasConnected = false;
    lastAttempt = 0;
    drawnStatic = false;
  }
  // Retry every 10s (WiFi.reconnect is non-blocking).
  if (millis() - lastAttempt >= 10000) {
    lastAttempt = millis();
    mlog.println("[WIFI] attempting reconnect...");
    WiFi.reconnect();
  }
}

void loop() {
  ESP.wdtFeed();           // keep the watchdog happy; a freeze stops this -> reset
  wifi_supervise();
  portal_handle();
  time_tick();             // periodic NTP resync / retry
  netfsm_tick();
  esphome_tick();
  monitors_tick();
  flight_tick();

  // --- Button: short press cycles screens, long press toggles the display ---
  // The ISR flags a falling edge; we then poll the pin to measure hold time.
  static bool btnHeld = false;
  static unsigned long btnStart = 0;
  static bool btnLongDone = false;
  if (btnToggle) {
    btnToggle = false;
    btnHeld = true;
    btnLongDone = false;
    btnStart = millis();
  }
  if (btnHeld) {
    bool down = (digitalRead(BTN_PIN) == LOW);
    if (down && !btnLongDone && millis() - btnStart >= BTN_LONG_MS) {
      // Long press fires as soon as the threshold is reached (no need to wait
      // for release). Toggle the whole display (screen + backlight) off/on and
      // clear any night override.
      btnLongDone = true;
      manualOff = !manualOff;
      nightOff = false;
      display_refresh();
      mlog.printf("[BTN] long press -> display %s\n", manualOff ? "OFF" : "ON");
    }
    if (!down) {
      // Released: if it never became a long press, treat as a screen cycle.
      if (!btnLongDone) {
        for (int n = 0; n < SCREEN_COUNT; n++) {
          screenIndex = (screenIndex + 1) % SCREEN_COUNT;
          bool disabled = !cfg.screen_enabled[screenIndex] ||
                          (screenIndex == 5 && cfg.flight_range <= 0);
          if (!disabled) break;
        }
        drawnStatic = false;
        mlog.printf("[BTN] screen %d/%d\n", screenIndex + 1, SCREEN_COUNT);
      }
      btnHeld = false;
    }
  }

  int h, m, s, dow, day, mon, yr;
  time_now(h, m, s, dow, day, mon, yr);

  // --- Auto night sleep (configurable hours) ---
  // On a day/night transition, update nightOff and clear the manual override so
  // the schedule takes back control. display_refresh() then blanks/restores the
  // whole display (screen contents + backlight) together.
  if (h != lastAutoHour) {
    lastAutoHour = h;
    int ns = cfg.night_start, ne = cfg.night_end;
    bool night = (ns == ne) ? false
               : (ns < ne)  ? (h >= ns && h < ne)          // same-day window
                            : (h >= ns || h < ne);          // wraps midnight
    if (night != nightOff) {
      nightOff = night;
      manualOff = false;
      display_refresh();
      mlog.printf("[AUTO] %s -> display %s\n", night ? "night" : "day", night ? "OFF" : "ON");
    }
  }

  // Periodic heap health log (throttled).
  static unsigned long lastHeapLog = 0;
  if (millis() - lastHeapLog > 60000) {
    lastHeapLog = millis();
    mlog.printf("[HEAP] free=%u frag=%u%% maxblk=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getHeapFragmentation(),
                (unsigned)ESP.getMaxFreeBlockSize());
  }

  // Non-blocking network updates (driven by netfsm_tick)
  bool dataUpdated = netfsm_updated();
  bool ehUpdated = esphome_updated();   // consume flag every loop
  bool flUpdated = flight_updated();    // consume flag every loop

  if (screenOn && ui_is_on()) {
    static int lastMin = -1;
    static unsigned long lastSec = 0;

    // Full screen redraw only on: switch, minute change, or new data.
    bool needRedraw = !drawnStatic;
    if (screenIndex == 0 && m != lastMin) needRedraw = true;
    if (dataUpdated) needRedraw = true;
    if (ehUpdated && screenIndex == 1) needRedraw = true;   // ESPHome screen live update
    if (flUpdated && screenIndex == 5) needRedraw = true;   // flight radar live update

    if (needRedraw) {
      draw_screen(h, m, s, dow, day, mon, yr);
      ui_wifi_indicator(WiFi.status() == WL_CONNECTED);
      lastMin = m;
      drawnStatic = true;
    } else if (screenIndex == 0) {
      // Seconds + uptime tick every second in their own boxes (flicker OK)
      if (millis() - lastSec >= 1000) {
        lastSec = millis();
        ui_draw_seconds(s);
        ui_draw_uptime(millis());
      }
      // Refresh only the flight box when new flight data arrives (no full redraw).
      if (flUpdated) ui_draw_flightinfo(flight_data());
    } else if (screenIndex == 5) {
      // Flight radar: tick the next-refresh countdown once per second.
      if (millis() - lastSec >= 1000) {
        lastSec = millis();
        ui_draw_flight_countdown();
      }
    } else if (screenIndex == 6) {
      // System screen: refresh dynamic values once per second (isolated boxes).
      if (millis() - lastSec >= 1000) {
        lastSec = millis();
        ui_system_update(WiFi.RSSI(), millis());
      }
    }
  }

  delay(200);
}


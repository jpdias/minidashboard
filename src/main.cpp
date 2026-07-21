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
#include "moon.h"
#include "control.h"

#define BTN_PIN D3
#define BL_PIN  D8            // backlight transistor gate/base (GPIO15; see README)
                              // NOTE: avoid D6/GPIO12 (HSPI MISO, reclaimed by TFT)
                              // and D0/GPIO16 (special I/O, won't drive cleanly).
                              // GPIO15 has a boot pulldown: backlight is off until
                              // firmware drives it HIGH, then fully controllable.

#define BTN_LONG_MS 600       // press >= this = long press (display on/off)

volatile bool btnToggle = false;
bool screenOn = true;
bool drawnStatic = false;
int screenIndex = 0;          // 0..N -> screens 1..N+1
const int SCREEN_COUNT = 7;   // 5 = flight radar, 6 = system info

// Display state: displayOn is the single source of truth for the physical
// on/off of the whole display (screen contents + backlight). A long press
// toggles it any time. A night schedule turns it off/on at its configured
// hours; each schedule transition edge applies its action regardless of any
// manual override, so the schedule always reclaims control at day<->night.
bool displayOn = true;

// Drive the backlight pin for the given state, honoring polarity.
static void backlight_write(bool on) {
  int level = (on == cfg.backlight_active_high) ? HIGH : LOW;
  digitalWrite(BL_PIN, level);
  mlog.printf("[BL] pin=%s (on=%d ah=%d ctl=%d)\n",
              level ? "HIGH" : "LOW", on, cfg.backlight_active_high, cfg.backlight_control);
}

// Apply the desired display state (screen + backlight). Idempotent: always
// drives both the panel and backlight to match `on`, so displayOn/screenOn and
// the physical state can never drift out of sync.
static void display_set(bool on) {
  displayOn = on;
  screenOn = on;
  if (on) {
    // The transistor switches the panel's common GND, so restore power FIRST,
    // then re-init the (cold-booted) controller and force a full redraw.
    backlight_write(true);
    ui_poweron();
    drawnStatic = false;
    mlog.println("[DISP] ON");
  } else {
    ui_poweroff();
    backlight_write(false);
    mlog.println("[DISP] OFF");
  }
}

// ---- Control API (shared by button, scheduler, and web/REST) ----------------

static const char* SCREEN_NAMES[SCREEN_COUNT] = {
  "Clock", "ESPHome", "Forecast", "Weather Detail", "Monitors", "Flight Radar", "System"
};

bool control_screen_enabled(int idx) {
  if (idx < 0 || idx >= SCREEN_COUNT) return false;
  if (!cfg.screen_enabled[idx]) return false;
  if (idx == 5 && cfg.flight_range <= 0) return false;  // flight radar off when range 0
  return true;
}

void control_display_set(bool on) { display_set(on); }
bool control_display_toggle() { display_set(!displayOn); return displayOn; }
bool control_display_is_on() { return displayOn; }

int control_screen_get() { return screenIndex; }
int control_screen_count() { return SCREEN_COUNT; }
const char* control_screen_name(int idx) {
  return (idx >= 0 && idx < SCREEN_COUNT) ? SCREEN_NAMES[idx] : "";
}

// Step to the next/prev enabled screen. dir = +1 or -1. Wakes the display.
static void screen_step(int dir) {
  for (int n = 0; n < SCREEN_COUNT; n++) {
    screenIndex = (screenIndex + dir + SCREEN_COUNT) % SCREEN_COUNT;
    if (control_screen_enabled(screenIndex)) break;
  }
  if (!displayOn) display_set(true);
  drawnStatic = false;
  mlog.printf("[CTL] screen %d/%d (%s)\n", screenIndex + 1, SCREEN_COUNT, control_screen_name(screenIndex));
}

void control_screen_next() { screen_step(+1); }
void control_screen_prev() { screen_step(-1); }

void control_screen_set(int idx) {
  if (!control_screen_enabled(idx)) return;
  screenIndex = idx;
  if (!displayOn) display_set(true);
  drawnStatic = false;
  mlog.printf("[CTL] screen set %d/%d (%s)\n", screenIndex + 1, SCREEN_COUNT, control_screen_name(screenIndex));
}

// True when the current hour falls inside the configured night window.
// A window where start == end means "disabled" (never night).
static bool schedule_is_night(int h) {
  int ns = cfg.night_start, ne = cfg.night_end;
  if (ns == ne) return false;
  if (ns < ne)  return (h >= ns && h < ne);   // same-day window
  return (h >= ns || h < ne);                 // wraps midnight
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

  // Drive the backlight pin HIGH immediately so the panel lights up during boot
  // regardless of config (default is an active-high low-side switch, e.g. BC547
  // on the GND line). Config may re-apply the correct state/polarity later.
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // Watchdog: HW watchdog auto-resets the chip if the loop freezes and stops
  // feeding it. Enable the software WDT with an explicit timeout as well.
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);   // ~8s; must call ESP.wdtFeed() regularly
  mlog.println("[WDT] watchdog enabled (8s)");

  config_load();
  portal_begin();          // connect wifi or spawn AP

  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), btn_isr, FALLING);

  // Apply the backlight ON state using the configured polarity now that config
  // is loaded (the pin was set HIGH early for the boot window).
  backlight_write(true);

  ui_begin();
  time_begin();
  time_update();           // NTP sync + TZ

   netfsm_begin((unsigned long)cfg.weather_interval * 1000UL);
   esphome_begin();
   monitors_begin();
   flight_begin();
   moon_begin();

   // Land on the first enabled screen (fall back to Clock if none enabled).
   screenIndex = 0;
   for (int i = 0; i < SCREEN_COUNT; i++) {
     bool disabled = !cfg.screen_enabled[i] || (i == 5 && cfg.flight_range <= 0);
     if (!disabled) { screenIndex = i; break; }
   }

   // --- Deterministic boot sequence ---
   // All the "once-per-day" data is fetched here, synchronously and in a fixed
   // order, with every step shown on screen. This removes the non-deterministic
   // FSM races (moon sometimes not loading, radar freezing) by ensuring each
   // fetch either completes or times out cleanly before the next begins, and that
   // the only two TLS sessions (moon, then flight) never overlap.
   const unsigned long BOOT_TIMEOUT = 30000;
   unsigned long bootStart = millis();

   ui_screen_boot("Fetching data...");
   ui_boot_step(0, "NTP sync", BOOT_WAIT);
   // Wait for a real clock before trusting any date-based fetch.
   while (!time_is_synced() && millis() - bootStart < 8000) {
     ESP.wdtFeed(); portal_handle(); time_tick();
     ui_boot_step(0, "NTP sync", BOOT_WAIT);
     delay(100);
   }
   ui_boot_step(0, "NTP sync", time_is_synced() ? BOOT_DONE : BOOT_FAIL);

   // 1) Weather + forecast + external IP (plain HTTP, no TLS contention).
   ui_boot_step(1, "Weather", BOOT_WAIT);
   ui_boot_step(2, "Forecast", BOOT_WAIT);
   unsigned long netT = millis();
   while (!netfsm_first_done() && millis() - netT < BOOT_TIMEOUT) {
     ESP.wdtFeed(); portal_handle(); time_tick(); netfsm_tick();
     delay(50);
   }
   ui_boot_step(1, "Weather",  net_weather().valid ? BOOT_DONE : BOOT_FAIL);
   ui_boot_step(2, "Forecast", net_forecast().valid ? BOOT_DONE : BOOT_FAIL);
   netfsm_mark_forecast_fresh();   // boot already fetched forecast; don't refetch now

   // 2) Moon (TLS) — first TLS use, so it runs alone. Deterministic + blocking.
   ui_boot_step(3, "Sun / Moon", BOOT_WAIT);
   bool moonOk = moon_fetch_blocking(12000);
   ui_boot_step(3, "Sun / Moon", moonOk ? BOOT_DONE : BOOT_FAIL);

   // 3) Flight radar first fetch (TLS) — moon has released the lock by now.
   if (cfg.flight_range > 0) {
     ui_boot_step(4, "Flight radar", BOOT_WAIT);
     bool flOk = flight_fetch_blocking(12000);
     ui_boot_step(4, "Flight radar", flOk ? BOOT_DONE : BOOT_FAIL);
   }

   ui_boot_step(5, "Ready", BOOT_DONE);
   mlog.println("[BOOT] sequence complete");
   delay(600);
}

void draw_screen(int h, int m, int s, int dow, int day, int mon, int yr) {
  const Weather &weather = net_weather();
  const Forecast &forecast = net_forecast();
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
  moon_tick();     // fetches sun/moon data once per local day (heap-guarded)

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
      // for release). Toggle the whole display on/off.
      btnLongDone = true;
      bool now = control_display_toggle();
      mlog.printf("[BTN] long press -> display %s\n", now ? "ON" : "OFF");
    }
    if (!down) {
      // Released: if it never became a long press, treat as a screen cycle.
      if (!btnLongDone) control_screen_next();
      btnHeld = false;
    }
  }

  int h, m, s, dow, day, mon, yr;
  time_now(h, m, s, dow, day, mon, yr);

  // --- Scheduled night off/on ---
  // Apply the schedule's action only on a day<->night transition edge. This lets
  // a manual long-press override persist between edges, while every edge still
  // reclaims control (turns off entering night, on entering day). Skip until NTP
  // has synced, since before that the clock reads epoch 00:xx (a false night).
  if (time_is_synced()) {
    static bool wasNight = false;
    static bool nightInit = false;
    bool night = schedule_is_night(h);
    if (!nightInit) {
      nightInit = true;
      wasNight = night;
      if (displayOn == night) {          // only act if it disagrees with schedule
        display_set(!night);
        mlog.printf("[SCHED] boot %s -> display %s\n", night ? "night" : "day", night ? "OFF" : "ON");
      }
    } else if (night != wasNight) {
      wasNight = night;
      display_set(!night);
      mlog.printf("[SCHED] %s -> display %s\n", night ? "night" : "day", night ? "OFF" : "ON");
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
  bool mnUpdated = moon_updated();      // consume flag every loop

  if (screenOn && ui_is_on()) {
    static int lastMin = -1;
    static unsigned long lastSec = 0;

    // Full screen redraw only on: switch, minute change, or new data.
    bool needRedraw = !drawnStatic;
    if (screenIndex == 0 && m != lastMin) needRedraw = true;
    if (dataUpdated) needRedraw = true;
    if (ehUpdated && screenIndex == 1) needRedraw = true;   // ESPHome screen live update
    if (flUpdated && screenIndex == 5) needRedraw = true;   // flight radar live update
    if (mnUpdated && screenIndex == 3) needRedraw = true;   // moon data arrived

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


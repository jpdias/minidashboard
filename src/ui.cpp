#include "ui.h"
#include "netmon.h"
#include "esphome.h"
#include "moon.h"
#include "nettime.h"
#include "config.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <time.h>

#define TFT_CS   D1
#define TFT_DC   D2
#define TFT_RST  D4

static Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
static bool ui_on = true;

// The built-in GFX font is 7-bit ASCII; common Unicode glyphs have no bitmap and
// render as garbage. Walk the string UTF-8 aware and map known sequences to ASCII,
// dropping any other non-ASCII bytes.
//   µ (0xC2 0xB5) -> u     ³ (0xC2 0xB3) -> 3     ² (0xC2 0xB2) -> 2
//   ° (0xC2 0xB0) -> space  – — (0xE2 0x80 0x93/94) -> -
void sanitize_ascii(char *s) {
  char *r = s, *w = s;
  while (*r) {
    unsigned char c = (unsigned char)*r;
    if (c < 0x80) {
      *w++ = c; r++;
    } else if (c == 0xC2 && (unsigned char)r[1] >= 0x80) {
      unsigned char n = (unsigned char)r[1];
      if (n == 0xB5) *w++ = 'u';
      else if (n == 0xB3) *w++ = '3';
      else if (n == 0xB2) *w++ = '2';
      else if (n == 0xB0) *w++ = ' ';   // degree -> space (drawn manually elsewhere)
      else if (n == 0xAE) { *w++ = '(', *w++ = 'R', *w++ = ')'; } // ®
      else if (n == 0xA9) { *w++ = '(', *w++ = 'C', *w++ = ')'; } // ©
      r += 2;
    } else if (c == 0xE2 && (unsigned char)r[1] == 0x80) {
      unsigned char n = (unsigned char)r[2];
      if (n == 0x93 || n == 0x94) *w++ = '-';  // – —
      else if (n == 0x99) { *w++ = '\''; *w++ = '\''; } // "
      r += 3;
    } else {
      r++;  // drop other non-ASCII byte(s)
    }
  }
  *w = 0;
}

// Print a temperature value followed by a hand-drawn degree glyph and unit,
// centered within [leftX, leftX+width] and auto-shrinking so it never wraps.
// Avoids the missing '°' bitmap in the default font.
static void ui_print_temp(float t, const char *unit, uint16_t col, int leftX, int width, int startSize = 3) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", t);
  int size = startSize;
  int numW, unitW, total;
  for (;;) {
    tft.setTextSize(size);
    // Default GFX font: 6px per character at text size (5px glyph + 1px space).
    numW = (int)strlen(buf) * 6 * size;
    unitW = (int)strlen(unit) * 6 * size;
    total = numW + 4 + unitW;          // 4 = degree dot + gap
    // width == 0 means left-align with no shrink; otherwise shrink to fit width.
    if (width == 0 || total <= width || size <= 1) break;
    size--;
  }
  int y = tft.getCursorY();
  // width == 0 -> left-align at leftX; otherwise center within [leftX, leftX+width].
  int startX = (width == 0) ? leftX : leftX + (width - total) / 2;
  tft.setCursor(startX, y);
  tft.print(buf);
  int cx = tft.getCursorX();
  tft.fillCircle(cx + 1, y + 2, 2, col);   // degree dot
  tft.setCursor(cx + 4, y);
  tft.print(unit);
}

void ui_begin() {
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(2);
  tft.fillScreen(ST7735_BLACK);
}

void ui_screen_loading(unsigned long waitedMs, unsigned long timeoutMs) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(2);
  tft.setCursor(18, 50);
  tft.print("miniDash");
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(14, 78);
  tft.print("Loading data...");
  // progress bar frame (filled portion updated separately)
  int w = 100, x = 14, y = 100;
  tft.drawRect(x, y, w, 8, ST7735_BLUE);
  ui_loading_update(waitedMs, timeoutMs);
}

// Redraw only the progress bar + timer (cheap, no full-screen flicker).
void ui_loading_update(unsigned long waitedMs, unsigned long timeoutMs) {
  int w = 100, x = 14, y = 100;
  int pct = (timeoutMs > 0) ? constrain(map((long)waitedMs, 0, (long)timeoutMs, 0, w), 0, w) : 0;
  tft.fillRect(x + 1, y + 1, w - 1, 6, ST7735_BLACK);          // clear bar
  tft.fillRect(x + 1, y + 1, pct, 6, ST7735_GREEN);            // fill bar
  tft.setCursor(14, 116);
  tft.fillRect(14, 116, 40, 8, ST7735_BLACK);                  // clear timer
  tft.setTextColor(ST7735_YELLOW);
  char buf[24];
  snprintf(buf, sizeof(buf), "%lus", (unsigned long)(waitedMs / 1000));
  tft.print(buf);
}

void ui_poweroff() {
  ui_on = false;
  tft.fillScreen(ST7735_BLACK);
}

void ui_screen_boot(const char *title) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("miniDash");
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 14);
  tft.print(title);
  tft.drawFastHLine(0, 24, 128, ST7735_BLUE);
  // reserve step lines 0..5 starting at y=32, 14px apart
}

void ui_boot_step(int idx, const char *label, BootStep st) {
  int y = 32 + idx * 14;
  tft.fillRect(2, y, 124, 12, ST7735_BLACK);          // clear the line
  uint16_t col = ST7735_WHITE;
  const char *mark = " ";
  if (st == BOOT_DONE) { col = ST7735_GREEN; mark = "+"; }
  else if (st == BOOT_FAIL) { col = ST7735_RED; mark = "!"; }
  else if (st == BOOT_WAIT) { col = ST7735_YELLOW; mark = "."; }
  tft.setTextColor(col);
  tft.setTextSize(1);
  tft.setCursor(4, y + 1);
  tft.print(mark);
  tft.setCursor(14, y + 1);
  tft.print(label);
}

void ui_poweron() {
  // The panel shares the common GND that the backlight transistor switches, so
  // turning it "off" fully removes power from the ST7735 controller too. On wake
  // it is a cold boot: give the rail a moment to come back up, then re-send the
  // full init sequence (which also pulses the hardware RST pin) before drawing.
  ui_on = true;
  delay(50);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(2);
  tft.fillScreen(ST7735_BLACK);
}

bool ui_is_on() {
  return ui_on;
}

// ---------- Weather icons (drawn with primitives) ----------
// Simple 16x16 glyphs centered at (cx,cy).
void ui_draw_icon(int code, int cx, int cy, uint16_t color) {
  tft.fillCircle(cx, cy, 16, ST7735_BLACK);  // clear icon box
  int sun = (code == 0 || code == 1);
  int cloudy = (code >= 2 && code <= 3) || (code >= 45);
  int rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  int snow = (code >= 71 && code <= 77) || (code >= 85 && code <= 86);
  int storm = (code >= 95);

  if (sun) {
    tft.fillCircle(cx, cy, 6, color);
    for (int a = 0; a < 360; a += 45) {
      int x1 = cx + cos(a * PI / 180) * 9;
      int y1 = cy + sin(a * PI / 180) * 9;
      int x2 = cx + cos(a * PI / 180) * 13;
      int y2 = cy + sin(a * PI / 180) * 13;
      tft.drawLine(x1, y1, x2, y2, color);
    }
  }
  if (cloudy || rain || snow || storm) {
    tft.fillRoundRect(cx - 9, cy - 3, 18, 9, 4, color);
    tft.fillRoundRect(cx - 5, cy - 7, 11, 8, 4, color);
  }
  if (rain) {
    for (int i = -6; i <= 6; i += 6)
      tft.drawLine(cx + i, cy + 7, cx + i - 2, cy + 13, ST7735_CYAN);
  }
  if (snow) {
    for (int i = -6; i <= 6; i += 6)
      tft.drawChar(cx + i - 3, cy + 8, '*', ST7735_WHITE, ST7735_BLACK, 1);
  }
  if (storm) {
    tft.drawLine(cx - 4, cy + 7, cx, cy + 12, ST7735_YELLOW);
    tft.drawLine(cx, cy + 12, cx - 3, cy + 16, ST7735_YELLOW);
  }
}

// Small offline marker bottom-left; only drawn when disconnected.
void ui_wifi_indicator(bool connected) {
  if (connected) return;
  tft.fillRect(0, 150, 60, 10, ST7735_BLACK);
  tft.setTextColor(ST7735_RED);
  tft.setTextSize(1);
  tft.setCursor(2, 151);
  tft.print("offline");
}

// Cellular-style signal bars for WiFi RSSI, drawn in the top bar.
// 5 ascending bars anchored at the bottom (y+9); filled ones = signal level.
void ui_draw_wifi_bars(int x, int y) {
  tft.fillRect(x, y, 20, 10, ST7735_BLACK);
  bool connected = (WiFi.status() == WL_CONNECTED);
  int bars = 0;
  if (connected) {
    int rssi = WiFi.RSSI();
    bars = constrain(map(rssi, -90, -50, 1, 5), 1, 5);
  }
  for (int i = 0; i < 5; i++) {
    int bh = 2 + i * 2;              // 2,4,6,8,10 px tall
    int bx = x + i * 4;
    int bytop = y + (10 - bh);
    uint16_t col = (i < bars) ? ST7735_GREEN
                 : (connected ? tft.color565(0, 60, 0) : ST7735_RED);
    tft.fillRect(bx, bytop, 3, bh, col);
  }
}

// ---------- Screen indicator ----------
// Also draws the shared WiFi signal bars so they appear on every screen.
void ui_screen_tag(int idx, int total) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d/%d", idx, total);
  tft.fillRect(74, 0, 54, 12, ST7735_BLACK);
  ui_draw_wifi_bars(76, 1);
  // Time-sync dot between the WiFi bars and the counter.
  tft.fillCircle(101, 5, 3, time_is_synced() ? ST7735_GREEN : ST7735_RED);
  tft.setTextColor(ST7735_BLUE);
  tft.setTextSize(1);
  // Right-align the counter against the screen edge (each char is 6px).
  tft.setCursor(124 - (int)strlen(buf) * 6, 2);
  tft.print(buf);
}

// ---------- Screen 1: Clock ----------
// HH:MM fills the width (size 3), :SS at far right (size 2) on the same line.
void ui_draw_clock_static(int h, int m, int dow, int day, int mon, int yr) {
  char buf[20];
  tft.fillRect(0, 0, 128, 52, ST7735_BLACK);

  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  // Two-digit year keeps the date compact so it clears the top-bar widgets.
  snprintf(buf, sizeof(buf), "%s %02d/%02d/%02d", dow_name(dow), day, mon, yr % 100);
  tft.print(buf);

  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(3);
  tft.setCursor(2, 16);
  snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  tft.print(buf);

  ui_draw_seconds(0);
}

void ui_draw_clock(int h, int m, int s, int dow, int day, int mon, int yr) {
  ui_draw_clock_static(h, m, dow, day, mon, yr);
  ui_draw_seconds(s);
}

void ui_draw_seconds(int s) {
  char buf[6];
  snprintf(buf, sizeof(buf), ":%02d", s);
  // isolated box just right of HH:MM (size3 ends ~x90), fits within 128px
  tft.fillRect(90, 16, 38, 22, ST7735_BLACK);
  tft.setTextColor(ST7735_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(92, 18);
  tft.print(buf);
}

void ui_draw_uptime(unsigned long uptime) {
  char buf[16];
  // Must match the uptime position drawn by ui_draw_metrics (y=138) so the
  // per-second tick and full redraws don't disagree and cause a vertical jump.
  tft.fillRect(0, 138, 128, 10, ST7735_BLACK);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 138);
  unsigned long up = uptime / 1000;
  snprintf(buf, sizeof(buf), "up %02lu:%02lu:%02lu",
           up / 3600, (up % 3600) / 60, up % 60);
  tft.print(buf);
}

// Small top-view plane glyph centered on (cx,cy).
static void draw_plane_icon(int cx, int cy, uint16_t col) {
  tft.drawFastVLine(cx, cy - 3, 8, col);      // fuselage
  tft.drawFastHLine(cx - 3, cy, 7, col);      // main wings
  tft.drawFastHLine(cx - 1, cy + 4, 3, col);  // tailplane
}

// Auto-refreshing closest-flight readout on a single line at the bottom.
// Drawn in its own isolated box so it can update without a full-screen redraw.
void ui_draw_flightinfo(const FlightData &fd) {
  const int by = 150, bh = 10;
  tft.fillRect(0, by, 128, bh, ST7735_BLACK);
  draw_plane_icon(5, by + 4, ST7735_CYAN);

  tft.setTextSize(1);
  tft.setCursor(14, by + 1);
  char buf[24];
  if (!fd.valid) {
    tft.setTextColor(ST7735_WHITE);
    tft.print("flights...");
    return;
  }
  if (fd.count == 0) {
    tft.setTextColor(ST7735_WHITE);
    tft.print("no flights");
    return;
  }
  const FlightAc &c = fd.ac[0];
  tft.setTextColor(ui_flight_tag_color(c.tag));
  snprintf(buf, sizeof(buf), "%s ", c.tag);
  tft.print(buf);
  tft.setTextColor(ST7735_YELLOW);
  tft.print(c.flight[0] ? c.flight : "----");
  tft.setTextColor(ST7735_WHITE);
  snprintf(buf, sizeof(buf), " %.0fnm", c.dst);
  tft.print(buf);
}

void ui_screen_clock(int h, int m, int s, int dow, int day, int mon, int yr,
                     const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime) {
  ui_draw_clock_static(h, m, dow, day, mon, yr);
  ui_draw_seconds(s);
  ui_draw_weather(w);
  ui_draw_flightinfo(flight_data());
  ui_draw_metrics(metrics, rssi, intIp, extIp, uptime);
  ui_screen_tag(1, 7);
}

// ---------- Screen 2: 3-day forecast ----------
void ui_screen_forecast(int h, int m, int s, const Forecast &f) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Forecast");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  char buf[24];
  time_t base = time(nullptr);
  const int rowH = 46;
  int y = 20;
  for (int i = 0; i < 3; i++) {
    const DayForecast &d = f.days[i];
    int cy = y + rowH / 2 - 2;               // vertical center of the row
    if (d.valid) {
      // Left: weather icon.
      ui_draw_icon(d.code, 20, cy, ST7735_YELLOW);
      // Middle-top: weekday label.
      time_t dt = base + (time_t)i * 86400;
      const struct tm *tmv = localtime(&dt);
      tft.setTextColor(ST7735_CYAN);
      tft.setTextSize(1);
      tft.setCursor(44, y + 6);
      tft.print((i == 0) ? "Today" : dow_name(tmv->tm_wday));
      // Top-right: high/low temps in green, aligned with the weekday.
      tft.setTextColor(ST7735_GREEN);
      snprintf(buf, sizeof(buf), "%.0f/%.0f C", d.tmax, d.tmin);
      int tw = (int)strlen(buf) * 6;
      tft.setCursor(126 - tw, y + 6);
      tft.print(buf);
      // Bottom: condition text spanning the width under the weekday.
      tft.setTextColor(0xAD55);
      tft.setCursor(44, y + 24);
      tft.print(weather_icon(d.code));
    } else {
      tft.setTextColor(ST7735_WHITE);
      tft.setCursor(44, cy);
      tft.print("--");
    }
    if (i < 2) tft.drawFastHLine(0, y + rowH - 2, 128, 0x2104);
    y += rowH;
  }
  ui_screen_tag(3, 7);
}

// ---------- Screen: ESPHome sensors (2nd) ----------
void ui_screen_esphome() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("ESPHome");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  char buf[32];
  int y = 22;
  for (int i = 0; i < esphome_count(); i++) {
    const EspHomeState &s = esphome_state(i);
    tft.setTextColor(ST7735_WHITE);
    tft.setTextSize(1);
    tft.setCursor(2, y);
     if (s.valid) {
      tft.print(s.name);
      tft.setCursor(2, y + 12);
      tft.setTextColor(ST7735_GREEN);
      tft.setTextSize(2);
      // Temperature sensor carries a 'C' unit -> draw a proper degree glyph,
      // left-aligned at size 2 like the other sensors.
      if (strstr(s.state, "C") && strchr(s.state, '.')) {
        float v = atof(s.state);
        snprintf(buf, sizeof(buf), "%.1f", v);
        tft.print(buf);
        int cx = tft.getCursorX();
        int cy = tft.getCursorY();
        tft.fillCircle(cx + 2, cy + 2, 2, ST7735_GREEN);   // degree dot
        tft.setCursor(cx + 6, cy);
        tft.print("C");
      } else {
        snprintf(buf, sizeof(buf), "%s", s.state);
        tft.print(buf);
      }
    } else {
      tft.print(s.name);
      tft.setCursor(2, y + 12);
      tft.setTextColor(ST7735_RED);
      tft.setTextSize(1);
      tft.print("-");
    }
    y += 32;
  }
  ui_screen_tag(2, 7);
}

// ---------- Screen 4: Weather detail ----------
// Draw a small moon phase glyph at (cx,cy) radius r. phase 0/1=new, 0.5=full.
// The lit fraction grows from the right (waxing) to full, then shrinks from the
// right (waning), mimicking the northern-hemisphere appearance.
static void ui_draw_moon(int cx, int cy, int r, float phase) {
  tft.fillCircle(cx, cy, r, ST7735_BLACK);     // dark disc (new)
  tft.drawCircle(cx, cy, r, 0x39C7);           // faint outline
  // Illuminated fraction and which limb is lit.
  bool waxing = phase < 0.5f;
  float k = cosf(2.0f * (float)M_PI * phase);  // +1 new .. -1 full
  // For each scanline, the terminator x-offset from center is k*sqrt(r^2-y^2).
  for (int y = -r; y <= r; y++) {
    int half = (int)(sqrtf((float)(r * r - y * y)) + 0.5f);
    int term = (int)(k * half + 0.5f);
    int x0, x1;
    if (waxing) { x0 = term; x1 = half; }      // lit on the right
    else        { x0 = -half; x1 = -term; }    // lit on the left
    if (x1 >= x0)
      tft.drawFastHLine(cx + x0, cy + y, x1 - x0 + 1, ST7735_WHITE);
  }
}

void ui_screen_detail(int h, int m, int s, const Weather &w) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Weather Now");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  if (w.valid) {
    char buf[24];
    // Line 1: weather icon + condition text.
    ui_draw_icon(w.code, 22, 38, ST7735_YELLOW);
    tft.setTextColor(0xAD55);
    tft.setTextSize(1);
    tft.setCursor(44, 34);
    tft.print(w.desc);

    // Line 2: temperature (green) then humidity (cyan), size 2, on one row.
    tft.setTextSize(2);
    tft.setTextColor(ST7735_GREEN);
    tft.setCursor(6, 60);
    ui_print_temp(w.temp, "C", ST7735_GREEN, 6, 0, 2);
    tft.setTextColor(ST7735_CYAN);
    snprintf(buf, sizeof(buf), "%d%%", w.humidity);
    int hw = (int)strlen(buf) * 12;           // size 2 = 12px per char
    tft.setCursor(122 - hw, 60);
    tft.print(buf);

    tft.drawFastHLine(0, 86, 128, 0x2104);
    tft.setTextSize(1);                        // back to small text for sun/moon

    const MoonInfo &mn = moon_data();
    const char *sr = mn.valid && mn.sunrise[0] ? mn.sunrise : (w.sunrise[0] ? w.sunrise : "--:--");
    const char *ss = mn.valid && mn.sunset[0]  ? mn.sunset  : (w.sunset[0]  ? w.sunset  : "--:--");

    // --- Sun row (arrow-up = rise, arrow-down = set) ---
    tft.setTextColor(ST7735_YELLOW);
    tft.setCursor(2, 98);
    tft.print("Sun");
    tft.fillTriangle(34, 97, 30, 103, 38, 103, ST7735_YELLOW);  // up = rise
    tft.setTextColor(ST7735_WHITE);
    tft.setCursor(42, 98);
    tft.print(sr);
    tft.fillTriangle(84, 103, 80, 97, 88, 97, 0xFC00);          // down = set
    tft.setCursor(92, 98);
    tft.print(ss);

    tft.drawFastHLine(0, 114, 128, 0x2104);                     // sun/moon divider

    // --- Moon row ---
    ui_draw_moon(10, 132, 9, mn.valid ? mn.phase : 0.0f);
    tft.setTextColor(ST7735_WHITE);
    tft.setCursor(24, 122);
    if (mn.valid) {
      snprintf(buf, sizeof(buf), "%s %d%%", mn.name, mn.illum);
    } else {
      snprintf(buf, sizeof(buf), "moon ...");
    }
    tft.print(buf);

    const char *mr = mn.valid && mn.moonrise[0] ? mn.moonrise : "--:--";
    const char *ms = mn.valid && mn.moonset[0]  ? mn.moonset  : "--:--";
    tft.fillTriangle(30, 143, 26, 149, 34, 149, 0xC618);       // up = rise
    tft.setTextColor(ST7735_WHITE);
    tft.setCursor(38, 144);
    tft.print(mr);
    tft.fillTriangle(84, 149, 80, 143, 88, 143, 0xC618);       // down = set
    tft.setCursor(92, 144);
    tft.print(ms);
  } else {
    tft.setCursor(8, 44);
    tft.print("no data");
  }
  ui_screen_tag(4, 7);
}

// ---------- Screen 5: Website monitors ----------
void ui_screen_monitors() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Monitors");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  char buf[32];
  int y = 22;
  for (int i = 0; i < MONITOR_MAX; i++) {
    if (cfg.monitors[i][0] == 0) continue;
    const MonitorState &m = monitors[i];
    // status dot
    tft.fillCircle(8, y + 4, 4, m.online ? ST7735_GREEN : ST7735_RED);
    // host
    tft.setTextColor(ST7735_WHITE);
    tft.setTextSize(1);
    tft.setCursor(18, y);
    tft.print(cfg.monitors[i]);
    // latency / status + uptime
    tft.setCursor(18, y + 12);
    if (m.online) {
      snprintf(buf, sizeof(buf), "%dms  up %.0f%%", m.latency, monitor_uptime_pct(i));
    } else {
      snprintf(buf, sizeof(buf), "DOWN  up %.0f%%", monitor_uptime_pct(i));
    }
    tft.print(buf);
    y += 30;
  }
  ui_screen_tag(5, 7);
}

// Color code for a flight class tag.
uint16_t ui_flight_tag_color(const char *tag) {
  if (!strcmp(tag, "MIL")) return ST7735_RED;
  if (!strcmp(tag, "HEL")) return ST7735_MAGENTA;
  if (!strcmp(tag, "COM")) return ST7735_CYAN;
  if (!strcmp(tag, "LGT")) return ST7735_GREEN;
  if (!strcmp(tag, "GLI")) return tft.color565(0, 255, 180);   // teal
  if (!strcmp(tag, "BAL")) return tft.color565(255, 140, 0);   // orange
  if (!strcmp(tag, "ULT")) return tft.color565(180, 255, 0);   // lime
  if (!strcmp(tag, "UAV")) return tft.color565(255, 0, 140);   // pink
  return ST7735_WHITE;   // CIV / unknown
}

// ---------- Screen: Flight radar ----------
void ui_screen_flight(const FlightData &fd, int rangeNm) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Flights");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  const int cx = 64, cy = 82, R = 60;

  if (rangeNm <= 0) {
    tft.setTextColor(ST7735_WHITE);
    tft.setCursor(10, 70);
    tft.print("Disabled (set range)");
    ui_screen_tag(6, 7);
    return;
  }

  // Concentric range rings (outer = full range, inner = half).
  const uint16_t GRID = tft.color565(0, 60, 0);
  tft.drawCircle(cx, cy, R, GRID);
  tft.drawCircle(cx, cy, R / 2, GRID);
  tft.drawFastHLine(cx - R, cy, 2 * R, GRID);
  tft.drawFastVLine(cx, cy - R, 2 * R, GRID);
  tft.fillCircle(cx, cy, 2, ST7735_CYAN);   // you
  // North marker
  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(1);
  tft.setCursor(cx - 2, cy - R - 9);
  tft.print("N");

  // Aircraft: place by bearing (dir) and distance (dst) scaled to range.
  for (int i = 0; i < fd.count; i++) {
    const FlightAc &a = fd.ac[i];
    float rr = a.dst / (float)rangeNm;
    if (rr > 1.0f) rr = 1.0f;
    float pr = rr * R;
    float br = a.dir * 0.017453293f;          // bearing -> radians
    int ax = cx + (int)(pr * sinf(br));
    int ay = cy - (int)(pr * cosf(br));       // north = up
    uint16_t col = (i == 0) ? ST7735_YELLOW : ui_flight_tag_color(a.tag);
    tft.fillCircle(ax, ay, 2, col);
    // heading line
    if (a.track >= 0) {
      float tr = a.track * 0.017453293f;
      int hx = ax + (int)(6 * sinf(tr));
      int hy = ay - (int)(6 * cosf(tr));
      tft.drawLine(ax, ay, hx, hy, col);
    }
  }

  // Footer: count + closest flight details.
  char buf[32];
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, cy + R + 6);
  if (!fd.valid) {
    tft.print("no data");
  } else if (fd.count == 0) {
    snprintf(buf, sizeof(buf), "%dnm: no traffic", rangeNm);
    tft.print(buf);
  } else {
    const FlightAc &c = fd.ac[0];
    snprintf(buf, sizeof(buf), "[%s] %s %.0fnm",
             c.tag, c.flight[0] ? c.flight : "----", c.dst);
    tft.setTextColor(ST7735_YELLOW);
    tft.print(buf);
    tft.setTextColor(ST7735_WHITE);
    tft.setCursor(2, cy + R + 18);
    snprintf(buf, sizeof(buf), "%dft  %d/%d ac", c.alt, fd.count, fd.total);
    tft.print(buf);
  }

  ui_draw_flight_countdown();
  ui_screen_tag(6, 7);
}

// Next-refresh countdown in the bottom-right corner of the radar.
// Fixed-width clear so shrinking strings (e.g. "10s" -> "9s") don't leave
// stale digits behind. Right-aligned within a 24px box.
void ui_draw_flight_countdown() {
  char buf[12];
  int secs = flight_next_refresh_secs();
  if (secs > 0) snprintf(buf, sizeof(buf), "%ds", secs);
  else          snprintf(buf, sizeof(buf), "...");
  const int boxW = 24;
  const int boxX = 128 - boxW;
  tft.fillRect(boxX, 150, boxW, 10, ST7735_BLACK);
  int w = (int)strlen(buf) * 6;
  tft.setTextSize(1);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(128 - w, 150);
  tft.print(buf);
}

// ---------- Screen: System / technical info ----------
// Static labels + IP/version; dynamic values refreshed by ui_system_update.
void ui_screen_system(int rssi, String intIp, unsigned long uptime) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("System");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 20);  tft.print("Heap");
  tft.setCursor(2, 32);  tft.print("MaxBlk");
  tft.setCursor(2, 44);  tft.print("Frag");
  tft.setCursor(2, 56);  tft.print("WiFi");
  tft.setCursor(2, 68);  tft.print("SSID");
  tft.setCursor(2, 80);  tft.print("IP");
  tft.setCursor(2, 92);  tft.print("MAC");
  tft.setCursor(2, 104); tft.print("CPU");
  tft.setCursor(2, 116); tft.print("Flash");
  tft.setCursor(2, 128); tft.print("Up");

  // Static-ish values (column at x=44 to leave room for wide values).
  tft.setTextColor(ST7735_GREEN);
  char buf[24];
  String ssid = WiFi.SSID();
  if (ssid.length() > 13) ssid = ssid.substring(0, 13);
  tft.setCursor(44, 68);  tft.print(ssid);
  tft.setCursor(44, 80);  tft.print(intIp);
  // Only the last 3 MAC octets fit at this size.
  String mac = WiFi.macAddress();
  tft.setCursor(44, 92);  tft.print(mac.length() >= 8 ? mac.substring(9) : mac);
  tft.setCursor(44, 104); snprintf(buf, sizeof(buf), "%dMHz", ESP.getCpuFreqMHz()); tft.print(buf);
  tft.setCursor(44, 116); snprintf(buf, sizeof(buf), "%uKB", ESP.getFlashChipRealSize() / 1024); tft.print(buf);

  ui_system_update(rssi, uptime);
  ui_screen_tag(7, 7);
}

void ui_system_update(int rssi, unsigned long uptime) {
  char buf[24];
  tft.setTextSize(1);
  tft.setTextColor(ST7735_GREEN);

  uint32_t heap = ESP.getFreeHeap();
  uint32_t maxblk = ESP.getMaxFreeBlockSize();
  uint8_t frag = ESP.getHeapFragmentation();

  tft.fillRect(44, 20, 84, 10, ST7735_BLACK);
  tft.setCursor(44, 20); snprintf(buf, sizeof(buf), "%uKB", heap / 1024); tft.print(buf);
  tft.fillRect(44, 32, 84, 10, ST7735_BLACK);
  tft.setCursor(44, 32); snprintf(buf, sizeof(buf), "%uKB", maxblk / 1024); tft.print(buf);
  tft.fillRect(44, 44, 84, 10, ST7735_BLACK);
  tft.setCursor(44, 44); snprintf(buf, sizeof(buf), "%u%%", frag); tft.print(buf);

  tft.fillRect(44, 56, 84, 10, ST7735_BLACK);
  tft.setCursor(44, 56);
  if (WiFi.status() == WL_CONNECTED) { snprintf(buf, sizeof(buf), "%ddBm", rssi); tft.print(buf); }
  else { tft.setTextColor(ST7735_RED); tft.print("offline"); tft.setTextColor(ST7735_GREEN); }

  tft.fillRect(24, 128, 104, 10, ST7735_BLACK);
  unsigned long up = uptime / 1000;
  tft.setCursor(24, 128);
  snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu",
           up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60);
  tft.print(buf);
}

// ---------- Legacy combined (unused by loop, kept for reference) ----------
void ui_draw_weather(const Weather &w) {
  char buf[20];
  tft.fillRect(0, 52, 128, 56, ST7735_BLACK);
  tft.drawFastHLine(0, 52, 128, ST7735_BLUE);

  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(2);
  tft.setCursor(2, 58);
  if (w.valid) {
    ui_print_temp(w.temp, "C", ST7735_GREEN, 2, 0);
  } else {
    tft.print("--.-");
    int cx = tft.getCursorX(), cy = tft.getCursorY();
    tft.fillCircle(cx + 2, cy + 2, 2, ST7735_GREEN);
    tft.setCursor(cx + 6, cy);
    tft.print("C");
  }

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(4, 80);
  if (w.valid) {
    char dbuf[32];
    strncpy(dbuf, w.desc, sizeof(dbuf) - 1);
    dbuf[sizeof(dbuf) - 1] = 0;
    tft.print(dbuf);
    tft.setCursor(4, 92);
    snprintf(buf, sizeof(buf), "Hum %d%%", w.humidity);
    tft.print(buf);
  } else {
    tft.print("weather...");
  }
}

void ui_draw_metrics(bool metrics, int rssi, String intIp, String extIp, unsigned long uptime) {
  tft.fillRect(0, 100, 128, 46, ST7735_BLACK);
  if (!metrics) return;

  tft.drawFastHLine(0, 102, 128, ST7735_BLUE);
  tft.setTextSize(1);
  char buf[24];
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 108);
  tft.print("LAN ");
  tft.print(intIp);
  tft.setCursor(2, 118);
  tft.print("WAN ");
  tft.print(extIp.length() ? extIp : "-");
  tft.setCursor(2, 138);
  unsigned long up = uptime / 1000;
  snprintf(buf, sizeof(buf), "up %02lu:%02lu:%02lu",
           up / 3600, (up % 3600) / 60, up % 60);
  tft.print(buf);
}

void ui_draw(int h, int m, int s, int dow, int day, int mon, int yr,
             const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime) {
  ui_screen_clock(h, m, s, dow, day, mon, yr, w, metrics, rssi, intIp, extIp, uptime);
}

void ui_draw_full(int h, int m, int s, int dow, int day, int mon, int yr,
                   const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime) {
  ui_screen_clock(h, m, s, dow, day, mon, yr, w, metrics, rssi, intIp, extIp, uptime);
}

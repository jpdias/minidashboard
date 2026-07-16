#include "ui.h"
#include "netmon.h"
#include "esphome.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

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
static void ui_print_temp(float t, const char *unit, uint16_t col, int leftX, int width) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", t);
  int size = 3;
  int numW, unitW, total;
  for (;;) {
    tft.setTextSize(size);
    // Default GFX font: 6px per character at text size (5px glyph + 1px space).
    numW = (int)strlen(buf) * 6 * size;
    unitW = (int)strlen(unit) * 6 * size;
    total = numW + 4 + unitW;          // 4 = degree dot + gap
    if (total <= width || size <= 1) break;
    size--;
  }
  int y = tft.getCursorY();
  int startX = leftX + (width - total) / 2;
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
  tft.print("miniTV");
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
  snprintf(buf, sizeof(buf), "%.0lds", (unsigned long)(waitedMs / 1000));
  tft.print(buf);
}

void ui_poweroff() {
  ui_on = false;
  tft.fillScreen(ST7735_BLACK);
}

void ui_poweron() {
  ui_on = true;
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

// ---------- Screen indicator ----------
void ui_screen_tag(int idx, int total) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d/%d", idx, total);
  tft.fillRect(98, 0, 30, 12, ST7735_BLACK);
  tft.setTextColor(ST7735_BLUE);
  tft.setTextSize(1);
  tft.setCursor(100, 2);
  tft.print(buf);
}

// ---------- Screen 1: Clock ----------
// HH:MM fills the width (size 3), :SS at far right (size 2) on the same line.
void ui_draw_clock_static(int h, int m, int dow, int day, int mon, int yr) {
  char buf[20];
  tft.fillRect(0, 0, 128, 50, ST7735_BLACK);

  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  snprintf(buf, sizeof(buf), "%s %02d/%02d/%04d", dow_name(dow), day, mon, yr);
  tft.print(buf);

  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(3);
  tft.setCursor(0, 16);
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
  tft.fillRect(0, 138, 128, 16, ST7735_BLACK);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 144);
  unsigned long up = uptime / 1000;
  snprintf(buf, sizeof(buf), "up %02lu:%02lu:%02lu",
           up / 3600, (up % 3600) / 60, up % 60);
  tft.print(buf);
}

void ui_screen_clock(int h, int m, int s, int dow, int day, int mon, int yr,
                     const Weather &w, bool metrics, int rssi, String intIp, String extIp, unsigned long uptime) {
  ui_draw_clock_static(h, m, dow, day, mon, yr);
  ui_draw_seconds(s);
  ui_draw_weather(w);
  ui_draw_metrics(metrics, rssi, intIp, extIp, uptime);
  ui_screen_tag(1, 6);
}

// ---------- Screen 2: 3-day forecast ----------
void ui_screen_forecast(int h, int m, int s, const Forecast &f) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("3-Day Forecast");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  char buf[24];
  int x = 6;
  for (int i = 0; i < 3; i++) {
    const DayForecast &d = f.days[i];
    tft.fillRect(x - 2, 18, 40, 138, ST7735_BLACK);
    if (d.valid) {
      ui_draw_icon(d.code, x + 18, 40, ST7735_YELLOW);
      tft.setTextColor(ST7735_WHITE);
      tft.setTextSize(1);
      tft.setCursor(x, 62);
      snprintf(buf, sizeof(buf), "Day %d", i + 1);
      tft.print(buf);
      tft.setTextColor(ST7735_GREEN);
      tft.setCursor(x, 78);
      snprintf(buf, sizeof(buf), "%.0f/%.0fC", d.tmax, d.tmin);
      tft.print(buf);
      tft.setTextColor(ST7735_WHITE);
      tft.setCursor(x, 94);
      tft.print(weather_icon(d.code));
    } else {
      tft.setCursor(x, 40);
      tft.print("--");
    }
    x += 42;
  }
  ui_screen_tag(3, 6);
}

// ---------- Screen 3: Network ----------
void ui_screen_network(int rssi, String intIp, String extIp, unsigned long uptime) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Network");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  char buf[32];
  int bars = constrain(map(rssi, -90, -30, 0, 5), 0, 5);
  tft.setTextColor(ST7735_MAGENTA);
  tft.setCursor(2, 22);
  tft.print("WiFi ");
  for (int i = 0; i < 5; i++) tft.print(i < bars ? "#" : ".");
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 38);
  tft.print("LAN ");
  tft.print(intIp);
  tft.setCursor(2, 52);
  tft.print("WAN ");
  tft.print(extIp.length() ? extIp : "-");

  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(2);
  tft.setCursor(2, 80);
  snprintf(buf, sizeof(buf), "%ld%% sig", (long)map(rssi, -90, -30, 0, 100));
  tft.print(buf);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 104);
  unsigned long up = uptime / 1000;
  snprintf(buf, sizeof(buf), "up %02lu:%02lu:%02lu",
           up / 3600, (up % 3600) / 60, up % 60);
  tft.print(buf);
  ui_screen_tag(4, 6);
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
      // Temperature sensor carries a 'C' unit -> draw a proper degree glyph.
      if (strstr(s.state, "C") && strchr(s.state, '.')) {
        float v = atof(s.state);
        if (v != 0.0f || strstr(s.state, "0.")) ui_print_temp(v, "C", ST7735_GREEN, 2, 124);
        else { snprintf(buf, sizeof(buf), "%s", s.state); tft.print(buf); }
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
  ui_screen_tag(2, 6);
}

// ---------- Screen 4: Weather detail ----------
void ui_screen_detail(int h, int m, int s, const Weather &w) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 2);
  tft.print("Weather Now");
  tft.drawFastHLine(0, 14, 128, ST7735_BLUE);

  if (w.valid) {
    ui_draw_icon(w.code, 64, 44, ST7735_YELLOW);
    char buf[24];
    tft.setTextColor(ST7735_GREEN);
    tft.setTextSize(3);
    tft.setCursor(0, 76);
    ui_print_temp(w.temp, "C", ST7735_GREEN, 0, 128);

    tft.setTextColor(ST7735_WHITE);
    tft.setTextSize(1);
    tft.setCursor(8, 104);
    tft.print(w.desc);
    tft.setCursor(8, 118);
    snprintf(buf, sizeof(buf), "Humidity %d%%", w.humidity);
    tft.print(buf);
    tft.setCursor(8, 132);
    snprintf(buf, sizeof(buf), "Updated %02d:%02d", h, m);
    tft.print(buf);
  } else {
    tft.setCursor(8, 44);
    tft.print("no data");
  }
  ui_screen_tag(5, 6);
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
  ui_screen_tag(6, 6);
}

// ---------- Legacy combined (unused by loop, kept for reference) ----------
void ui_draw_weather(const Weather &w) {
  char buf[20];
  tft.fillRect(0, 52, 128, 56, ST7735_BLACK);
  tft.drawFastHLine(0, 52, 128, ST7735_BLUE);

  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(2);
  tft.setCursor(4, 58);
  if (w.valid) {
    ui_print_temp(w.temp, "C", ST7735_GREEN, 4, 120);
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
  tft.setTextColor(ST7735_MAGENTA);
  tft.setTextSize(1);
  char buf[24];
  int bars = constrain(map(rssi, -90, -30, 0, 5), 0, 5);
  tft.setCursor(2, 108);
  tft.print("WiFi ");
  for (int i = 0; i < 5; i++) tft.print(i < bars ? "#" : ".");
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 118);
  tft.print("LAN ");
  tft.print(intIp);
  tft.setCursor(2, 128);
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

#include "logbuf.h"
#include "portal.h"
#include "nettime.h"
#include "control.h"
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>

WiFiManager wm;
static ESP8266WebServer server(80);
static ESP8266HTTPUpdateServer httpUpdater;

static String sanitize_hostname(const char *in);

// HTML escape for safe form values
static String htmlEscape(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') o += "&quot;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

// Send the "saved, rebooting" page inline (no LittleFS dependency) and reboot.
static void send_reboot_page();

// Render a set of input rows (one per non-empty value) as HTML, each sharing the
// same `name` (server collects them all). Values are HTML-escaped.
static String render_rows(const char* name, const char* values[], int n) {
  String h;
  for (int i = 0; i < n; i++) {
    if (!values[i] || !values[i][0]) continue;
    h += "<div class=\"row\"><input type=\"text\" name=\"";
    h += name;
    h += "\" value=\"";
    h += htmlEscape(values[i]);
    h += "\"><button type=\"button\" class=\"del\" onclick=\"this.parentNode.remove()\">x</button></div>";
  }
  return h;
}

// Monitor rows (one per configured host) as escaped HTML input rows.
static String monitor_rows() {
  const char* p[MONITOR_MAX];
  for (int i = 0; i < MONITOR_MAX; i++) p[i] = cfg.monitors[i];
  return render_rows("mon", p, MONITOR_MAX);
}

// ESPHome sensor rows: split the comma-separated string into escaped input rows.
static String esphome_rows() {
  String s = cfg.esphome_sensors;
  const char* parts[16]; int np = 0;
  int start = 0;
  for (int i = 0; i <= (int)s.length() && np < 16; i++) {
    if (i == (int)s.length() || s[i] == ',') {
      String tok = s.substring(start, i); start = i + 1; tok.trim();
      if (tok.length()) { char* b = (char*)malloc(tok.length() + 1); if (b) { strcpy(b, tok.c_str()); parts[np++] = b; } }
    }
  }
  String h = render_rows("ehs", parts, np);
  for (int i = 0; i < np; i++) free((void*)parts[i]);
  return h;
}

static String tz_options() {
  String o;
  for (int i = 0; i < tz_count(); i++) {
    const char* n = tz_name_at(i);
    o += "<option value=\"";
    o += n;
    o += "\"";
    if (strcmp(n, cfg.tz) == 0) o += " selected";
    o += ">";
    o += n;
    o += "</option>";
  }
  return o;
}

static void handle_style() {
  File f = LittleFS.open("/style.css", "r");
  if (!f) { server.send(404, "text/plain", "style.css not found"); return; }
  server.streamFile(f, "text/css");
  f.close();
}

static String log_text() {
  char* buf = logbuf_copy();
  String body = buf ? String(buf) : String("(empty)");
  delete[] buf;
  return body;
}

// Resolve a template token (name without the surrounding braces) to its HTML.
// Returns the literal "{{TOKEN}}" unchanged if the token is unknown, so typos in
// the template are visible rather than silently dropped.
static String resolve_token(const String& tok) {
  if (tok == "SSID") {
    String ssid = strlen(cfg.wifi_ssid) ? String(cfg.wifi_ssid) : WiFi.SSID();
    return htmlEscape(ssid);
  }
  if (tok == "HN")         return htmlEscape(cfg.hostname);
  if (tok == "LAT")        return String(cfg.lat, 4);
  if (tok == "LON")        return String(cfg.lon, 4);
  if (tok == "TZ_OPTIONS") return tz_options();
  if (tok == "WI")         return String(cfg.weather_interval);
  if (tok == "ME")         return String(cfg.show_metrics ? 1 : 0);
  if (tok == "NI")         return String(cfg.ntp_interval_min);
  if (tok == "NS")         return String(cfg.night_start);
  if (tok == "NE")         return String(cfg.night_end);
  if (tok == "EH")         return htmlEscape(cfg.esphome_host);
  if (tok == "MON_ROWS")   return monitor_rows();
  if (tok == "EHS_ROWS")   return esphome_rows();
  if (tok == "FR")         return String(cfg.flight_range);
  if (tok == "BLC")        return cfg.backlight_control ? "checked" : "";
  if (tok == "BLH")        return cfg.backlight_active_high ? "checked" : "";
  if (tok == "IP")         return WiFi.localIP().toString();
  if (tok == "LOG")        return htmlEscape(log_text());
  if (tok == "CFGJSON")    return htmlEscape(config_to_json());
  if (tok.startsWith("SC")) {
    int i = tok.substring(2).toInt();
    if (i >= 0 && i < SCREEN_MAX) return cfg.screen_enabled[i] ? "checked" : "";
  }
  return "{{" + tok + "}}";
}

// Serve the config page by streaming the template line-by-line and substituting
// {{TOKEN}} tokens as we go. Streaming keeps peak RAM low (one line + one token
// value at a time) instead of building a ~15-20KB String with many reallocations,
// which could silently truncate on a fragmented heap. No token spans a newline.
static void handle_root() {
  File f = LittleFS.open("/config.html", "r");
  if (!f) { server.send(500, "text/plain", "config.html missing (run uploadfs)"); return; }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  while (f.available()) {
    String line = f.readStringUntil('\n');
    // Keep the newline we consumed (readStringUntil strips it).
    if (f.available() || line.length()) line += "\n";

    int from = 0;
    int open;
    while ((open = line.indexOf("{{", from)) >= 0) {
      int close = line.indexOf("}}", open + 2);
      if (close < 0) break;  // no closing braces on this line; emit remainder as-is
      if (open > from) server.sendContent(line.substring(from, open));
      server.sendContent(resolve_token(line.substring(open + 2, close)));
      from = close + 2;
    }
    if (from < (int)line.length()) server.sendContent(line.substring(from));
  }
  f.close();
  server.sendContent("");  // end chunked response
}

// Raw log text for the auto-refreshing panel on the config page.
static void handle_logtext() {
  server.send(200, "text/plain", log_text());
}

// ---- Control API ------------------------------------------------------------

// Build the current control status as a small JSON document.
static String control_status_json() {
  String j = "{\"display_on\":";
  j += control_display_is_on() ? "true" : "false";
  j += ",\"screen\":";
  j += String(control_screen_get());
  j += ",\"count\":";
  j += String(control_screen_count());
  j += ",\"screens\":[";
  for (int i = 0; i < control_screen_count(); i++) {
    if (i) j += ",";
    j += "{\"name\":\"";
    j += htmlEscape(control_screen_name(i));
    j += "\",\"enabled\":";
    j += control_screen_enabled(i) ? "true" : "false";
    j += "}";
  }
  j += "]}";
  return j;
}

// GET /api/status -> current display + screen state.
static void handle_api_status() {
  server.send(200, "application/json", control_status_json());
}

// POST /api/display?state=on|off|toggle
static void handle_api_display() {
  String st = server.arg("state");
  if (st == "on")       control_display_set(true);
  else if (st == "off") control_display_set(false);
  else if (st == "toggle") control_display_toggle();
  else { server.send(400, "text/plain", "state must be on|off|toggle"); return; }
  server.send(200, "application/json", control_status_json());
}

// POST /api/screen?action=next|prev  OR  ?index=N
static void handle_api_screen() {
  if (server.hasArg("index")) {
    control_screen_set(server.arg("index").toInt());
  } else {
    String a = server.arg("action");
    if (a == "next") control_screen_next();
    else if (a == "prev") control_screen_prev();
    else { server.send(400, "text/plain", "action must be next|prev or provide index=N"); return; }
  }
  server.send(200, "application/json", control_status_json());
}

// GET /config.json -> pretty JSON view of the current config (read-only fetch).
static void handle_config_raw() {
  server.send(200, "application/json", config_to_json());
}

// POST /config.json -> apply an edited JSON document with strict validation.
static void handle_config_edit() {
  // Read the raw request body. ESP8266WebServer exposes it via arg("plain") for
  // non-form content types; fall back to draining the client stream directly so
  // an application/json POST is never seen as empty (which yields InvalidInput).
  String body = server.arg("plain");
  if (body.length() == 0 && server.hasArg("config")) body = server.arg("config");
  if (body.length() == 0) {
    WiFiClient& c = server.client();
    int len = server.arg("Content-Length").toInt();
    unsigned long t0 = millis();
    // Drain whatever remains on the client stream (the raw POST body). Bound by
    // Content-Length when present, and always by a size + time cap so we can't hang.
    while (c.connected() && c.available() && body.length() < 4096 &&
           (len <= 0 || (int)body.length() < len) &&
           (millis() - t0) < 2000) {
      body += (char)c.read();
    }
  }
  if (body.length() == 0) { server.send(400, "text/plain", "empty body"); return; }
  String err;
  if (!config_apply_json(body, err)) {
    server.send(400, "text/plain", "Config not applied: " + err);
    mlog.println("[CFG] edit rejected: " + err);
    return;
  }
  config_save();
  mlog.println("[CFG] applied edited config.json");
  send_reboot_page();
}

// Send the "saved, rebooting" page inline (no LittleFS dependency) and reboot.
// Inlined so a failed template read during the pre-reboot window can never leave
// the browser tab hanging on a half-open connection.
static void send_reboot_page() {
  const char* page =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Saved</title><link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
    "<h1>miniDash</h1>"
    "<div class=\"msg\"><strong>Saved.</strong><br>Rebooting... returning to config in "
    "<span id=\"c\">10</span>s.</div>"
    "<script>var n=10;var el=document.getElementById('c');"
    "var t=setInterval(function(){n--;if(el)el.textContent=n;"
    "if(n<=0){clearInterval(t);location.href='/';}},1000);</script>"
    "</body></html>";
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", page);
  server.client().flush();
  server.client().stop();
  yield();
  delay(600);
  ESP.restart();
}

// Sanitize a user hostname to a valid DNS label: lowercase alphanumerics and
// hyphens only, no leading/trailing hyphen, non-empty. Falls back to "minidash".
static String sanitize_hostname(const char *in) {
  String out;
  for (const char *p = in; *p && out.length() < 24; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
  }
  while (out.length() && out[0] == '-') out.remove(0, 1);
  while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
  if (out.length() == 0) out = "minidash";
  return out;
}

void portal_begin() {
  config_load();

  WiFi.hostname(sanitize_hostname(cfg.hostname));

  if (!LittleFS.begin()) {
    mlog.println("[PORTAL] LittleFS mount failed (run 'pio run -t uploadfs')");
  }

  WiFiManagerParameter p_lat("lat", "Latitude", String(cfg.lat, 4).c_str(), 10);
  WiFiManagerParameter p_lon("lon", "Longitude", String(cfg.lon, 4).c_str(), 10);
  WiFiManagerParameter p_tz("tz", "Timezone (e.g. Europe/Lisbon)", cfg.tz, 32);
  WiFiManagerParameter p_wi("wi", "Weather refresh (sec)", String(cfg.weather_interval).c_str(), 6);
  WiFiManagerParameter p_me("me", "Show system metrics (0/1)", String(cfg.show_metrics ? 1 : 0).c_str(), 2);
  WiFiManagerParameter p_eh("eh", "ESPHome host (e.g. ikea-hack.lan)", cfg.esphome_host, MONITOR_LEN);

  // Monitors: comma-separated hostnames, up to MONITOR_MAX
  char monBuf[256] = {0};
  int mp = 0;
  for (int i = 0; i < MONITOR_MAX; i++) {
    if (cfg.monitors[i][0]) {
      mp += snprintf(monBuf + mp, sizeof(monBuf) - mp, "%s,", cfg.monitors[i]);
    }
  }
  if (mp > 0) monBuf[mp - 1] = 0;  // trim trailing comma
  WiFiManagerParameter p_mon("mon", "Monitors (host1,host2,host3)", monBuf, 256);

  wm.addParameter(&p_lat);
  wm.addParameter(&p_lon);
  wm.addParameter(&p_tz);
  wm.addParameter(&p_wi);
  wm.addParameter(&p_me);
  wm.addParameter(&p_eh);
  wm.addParameter(&p_mon);

  bool res = wm.autoConnect("miniDash-Setup", "minidashpass");
  if (!res) {
    mlog.println("Failed to connect, restarting");
    ESP.restart();
  }

  // Persist the connected SSID into cfg so the config page shows it.
  if (WiFi.SSID().length()) strncpy(cfg.wifi_ssid, WiFi.SSID().c_str(), sizeof(cfg.wifi_ssid) - 1);

  // Read back custom parameters
  cfg.lat = String(p_lat.getValue()).toFloat();
  cfg.lon = String(p_lon.getValue()).toFloat();
  strncpy(cfg.tz, p_tz.getValue(), sizeof(cfg.tz) - 1);
  int wi = String(p_wi.getValue()).toInt();
  if (wi >= 60) cfg.weather_interval = wi;
  cfg.show_metrics = (String(p_me.getValue()).toInt() != 0);
  strncpy(cfg.esphome_host, p_eh.getValue(), sizeof(cfg.esphome_host) - 1);

  // Parse monitors (comma-separated)
  String m = String(p_mon.getValue());
  m.replace(" ", "");
  for (int i = 0; i < MONITOR_MAX; i++) cfg.monitors[i][0] = 0;
  int idx = 0, start = 0;
  for (unsigned int i = 0; i <= (unsigned int)m.length() && idx < MONITOR_MAX; i++) {
    if (i == (unsigned int)m.length() || m.charAt(i) == ',') {
      String host = m.substring(start, i);
      start = i + 1;
      if (host.length() > 0 && host.length() < MONITOR_LEN) {
        strncpy(cfg.monitors[idx], host.c_str(), MONITOR_LEN - 1);
        idx++;
      }
    }
  }

  config_save();

  // Always-on admin web UI (station mode)
  server.on("/", handle_root);
  server.on("/style.css", handle_style);
  server.on("/logtext", handle_logtext);
  server.on("/config.json", HTTP_GET, handle_config_raw);
  server.on("/config.json", HTTP_POST, handle_config_edit);
  server.on("/api/status", HTTP_GET, handle_api_status);
  server.on("/api/display", HTTP_POST, handle_api_display);
  server.on("/api/screen", HTTP_POST, handle_api_screen);
  httpUpdater.setup(&server);   // OTA: firmware + filesystem at /update
  server.begin();
  mlog.println("[PORTAL] admin UI started at http://" + WiFi.localIP().toString());

  // mDNS: reachable as http://<hostname>.local on the LAN.
  String host = sanitize_hostname(cfg.hostname);
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", 80);
    mlog.println("[MDNS] responder started at http://" + host + ".local");
  } else {
    mlog.println("[MDNS] responder failed to start");
  }
}

void portal_handle() {
  wm.process();
  server.handleClient();
  MDNS.update();
}

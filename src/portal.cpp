#include "logbuf.h"
#include "portal.h"
#include "nettime.h"
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

// Load a template file from LittleFS into a String.
static String loadTemplate(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

// Send the "saved, rebooting" page inline (no LittleFS dependency) and reboot.
static void send_reboot_page();

static String monitors_to_csv() {
  String m;
  for (int i = 0; i < MONITOR_MAX; i++) {
    if (cfg.monitors[i][0]) {
      m += cfg.monitors[i];
      m += "\n";
    }
  }
  return m;
}

static void handle_style() {
  File f = LittleFS.open("/style.css", "r");
  if (!f) { server.send(404, "text/plain", "style.css not found"); return; }
  server.streamFile(f, "text/css");
  f.close();
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

static String log_text() {
  char* buf = logbuf_copy();
  String body = buf ? String(buf) : String("(empty)");
  delete[] buf;
  return body;
}

static void handle_root() {
  String html = loadTemplate("/config.html");
  if (html.length() == 0) { server.send(500, "text/plain", "config.html missing (run uploadfs)"); return; }
  // WiFi credentials set via the WiFiManager AP portal live in the ESP's own
  // flash, not in cfg. Fall back to the actively-connected SSID.
  String ssid = strlen(cfg.wifi_ssid) ? String(cfg.wifi_ssid) : WiFi.SSID();
  html.replace("{{SSID}}", htmlEscape(ssid));
  html.replace("{{HN}}", htmlEscape(cfg.hostname));
  html.replace("{{LAT}}", String(cfg.lat, 4));
  html.replace("{{LON}}", String(cfg.lon, 4));
  html.replace("{{TZ_OPTIONS}}", tz_options());
  html.replace("{{WI}}", String(cfg.weather_interval));
  html.replace("{{ME}}", String(cfg.show_metrics ? 1 : 0));
  html.replace("{{NI}}", String(cfg.ntp_interval_min));
  html.replace("{{NS}}", String(cfg.night_start));
  html.replace("{{NE}}", String(cfg.night_end));
  html.replace("{{EH}}", htmlEscape(cfg.esphome_host));
  html.replace("{{EHS}}", htmlEscape(cfg.esphome_sensors));
  html.replace("{{MON}}", htmlEscape(monitors_to_csv()));
  html.replace("{{FR}}", String(cfg.flight_range));
  html.replace("{{BLC}}", cfg.backlight_control ? "checked" : "");
  html.replace("{{BLH}}", cfg.backlight_active_high ? "checked" : "");
  for (int i = 0; i < SCREEN_MAX; i++) {
    html.replace("{{SC" + String(i) + "}}", cfg.screen_enabled[i] ? "checked" : "");
  }
  html.replace("{{IP}}", WiFi.localIP().toString());
  html.replace("{{LOG}}", htmlEscape(log_text()));
  html.replace("{{CFGJSON}}", htmlEscape(config_to_json()));
  server.send(200, "text/html", html);
}

// Raw log text for the auto-refreshing panel on the config page.
static void handle_logtext() {
  server.send(200, "text/plain", log_text());
}

// GET /config.json -> pretty JSON view of the current config (read-only fetch).
static void handle_config_raw() {
  server.send(200, "application/json", config_to_json());
}

// POST /config.json -> apply an edited JSON document with strict validation.
static void handle_config_edit() {
  String body = server.arg("plain");
  if (body.length() == 0 && server.hasArg("config")) body = server.arg("config");
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

static void handle_save() {
  if (server.hasArg("ssid")) strncpy(cfg.wifi_ssid, server.arg("ssid").c_str(), sizeof(cfg.wifi_ssid) - 1);
  if (server.hasArg("pass")) {
    String p = server.arg("pass");
    if (p.length() > 0) strncpy(cfg.wifi_pass, p.c_str(), sizeof(cfg.wifi_pass) - 1);
  }
  if (server.hasArg("lat")) cfg.lat = server.arg("lat").toFloat();
  if (server.hasArg("lon")) cfg.lon = server.arg("lon").toFloat();
  if (server.hasArg("tz")) strncpy(cfg.tz, server.arg("tz").c_str(), sizeof(cfg.tz) - 1);
  if (server.hasArg("hn")) {
    String hn = sanitize_hostname(server.arg("hn").c_str());
    strncpy(cfg.hostname, hn.c_str(), sizeof(cfg.hostname) - 1);
  }
  if (server.hasArg("wi")) {
    int wi = server.arg("wi").toInt();
    if (wi >= 60) cfg.weather_interval = wi;
  }
  if (server.hasArg("me")) cfg.show_metrics = (server.arg("me").toInt() != 0);
  if (server.hasArg("ni")) { int ni = server.arg("ni").toInt(); if (ni >= 1) cfg.ntp_interval_min = ni; }
  if (server.hasArg("ns")) cfg.night_start = constrain(server.arg("ns").toInt(), 0, 23);
  if (server.hasArg("ne")) cfg.night_end = constrain(server.arg("ne").toInt(), 0, 23);
  if (server.hasArg("eh")) strncpy(cfg.esphome_host, server.arg("eh").c_str(), sizeof(cfg.esphome_host) - 1);
  if (server.hasArg("ehs")) {
    // Multiple inputs may share name="ehs" (one per row). Join them.
    String joined;
    for (int a = 0; a < server.args(); a++) {
      if (server.argName(a) == "ehs") {
        String v = server.arg(a);
        v.replace("\n", ","); v.replace("\r", "");
        if (v.length()) { joined += v; joined += ","; }
      }
    }
    if (joined.length() > 0) joined.remove(joined.length() - 1);
    strncpy(cfg.esphome_sensors, joined.c_str(), sizeof(cfg.esphome_sensors) - 1);
    cfg.esphome_sensors[sizeof(cfg.esphome_sensors) - 1] = 0;
  }
  if (server.hasArg("fr")) cfg.flight_range = constrain(server.arg("fr").toInt(), 0, 250);
  cfg.backlight_control = server.hasArg("blc");
  cfg.backlight_active_high = server.hasArg("blh");
  if (server.hasArg("scr")) {
    for (int i = 0; i < SCREEN_MAX; i++)
      cfg.screen_enabled[i] = server.hasArg("sc" + String(i));
  }
  if (server.hasArg("mon")) {
    // Multiple inputs may share name="mon" (one host per row). Collect them all.
    String m;
    for (int a = 0; a < server.args(); a++) {
      if (server.argName(a) == "mon") { String v = server.arg(a); v.replace("\n", ","); v.replace("\r", ""); m += v; m += ","; }
    }
    m.replace(" ", "");
    if (m.length() > 0) m.remove(m.length() - 1);
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
  }
  config_save();
  send_reboot_page();
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
  server.on("/save", HTTP_POST, handle_save);
  server.on("/logtext", handle_logtext);
  server.on("/config.json", HTTP_GET, handle_config_raw);
  server.on("/config.json", HTTP_POST, handle_config_edit);
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

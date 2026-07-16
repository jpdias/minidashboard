#include "logbuf.h"
#include "portal.h"
#include <ESP8266WebServer.h>

WiFiManager wm;
static ESP8266WebServer server(80);

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

static String monitors_to_csv() {
  String m;
  for (int i = 0; i < MONITOR_MAX; i++) {
    if (cfg.monitors[i][0]) {
      m += cfg.monitors[i];
      m += ",";
    }
  }
  if (m.length() > 0) m.remove(m.length() - 1);
  return m;
}

static void handle_root() {
  String mon = htmlEscape(monitors_to_csv());
  String ssid = htmlEscape(cfg.wifi_ssid);
  String tz = htmlEscape(cfg.tz);

  String html = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<title>miniTV Config</title></head><body style=\"font-family:sans-serif;max-width:420px;margin:auto;padding:12px\">"
                "<h2>miniTV Configuration</h2>"
                "<form method=\"post\" action=\"/save\">"
                "WiFi SSID:<br><input name=\"ssid\" value=\"" + ssid + "\" style=\"width:100%\"><br><br>"
                "WiFi Password:<br><input name=\"pass\" type=\"password\" placeholder=\"(unchanged if blank)\" style=\"width:100%\"><br><br>"
                "Latitude:<br><input name=\"lat\" value=\"" + String(cfg.lat, 4) + "\" style=\"width:100%\"><br><br>"
                "Longitude:<br><input name=\"lon\" value=\"" + String(cfg.lon, 4) + "\" style=\"width:100%\"><br><br>"
                "Timezone:<br><input name=\"tz\" value=\"" + tz + "\" style=\"width:100%\"><br><br>"
                "Weather refresh (sec, min 60):<br><input name=\"wi\" value=\"" + String(cfg.weather_interval) + "\" style=\"width:100%\"><br><br>"
                "Show metrics (0/1):<br><input name=\"me\" value=\"" + String(cfg.show_metrics ? 1 : 0) + "\" style=\"width:100%\"><br><br>"
                "ESPHome host (e.g. ikea-hack.lan):<br><input name=\"eh\" value=\"" + htmlEscape(cfg.esphome_host) + "\" style=\"width:100%\"><br><br>"
                "Monitors (comma separated):<br><input name=\"mon\" value=\"" + mon + "\" style=\"width:100%\"><br><br>"
                "<button type=\"submit\">Save &amp; Reboot</button>"
                "</form><hr><p>Device: " + WiFi.localIP().toString() +
                " &middot; <a href=\"/log\">Live log</a></p></body></html>";
  server.send(200, "text/html", html);
}

// Live serial terminal, like ESPHome devices expose. Auto-refreshes.
static void handle_log() {
  char* buf = logbuf_copy();
  String body = buf ? String(buf) : String("(empty)");
  delete[] buf;
  // escape HTML
  body.replace("&", "&amp;");
  body.replace("<", "&lt;");
  body.replace(">", "&gt;");
  String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<title>miniTV Log</title>"
                "<meta http-equiv=\"refresh\" content=\"2\">"
                "<style>body{background:#111;color:#0f0;font:12px monospace;white-space:pre-wrap;padding:8px}</style>"
                "</head><body>";
  html += body;
  html += "\n\n<a href=\"/\" style=\"color:#0ff\">back</a></body></html>";
  server.send(200, "text/html", html);
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
  if (server.hasArg("wi")) {
    int wi = server.arg("wi").toInt();
    if (wi >= 60) cfg.weather_interval = wi;
  }
  if (server.hasArg("me")) cfg.show_metrics = (server.arg("me").toInt() != 0);
  if (server.hasArg("eh")) strncpy(cfg.esphome_host, server.arg("eh").c_str(), sizeof(cfg.esphome_host) - 1);
  if (server.hasArg("mon")) {
    String m = server.arg("mon");
    m.replace(" ", "");
    for (int i = 0; i < MONITOR_MAX; i++) cfg.monitors[i][0] = 0;
    int idx = 0, start = 0;
    for (int i = 0; i <= m.length() && idx < MONITOR_MAX; i++) {
      if (i == m.length() || m.charAt(i) == ',') {
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
  server.send(200, "text/html", "<!DOCTYPE html><html><body style=\"font-family:sans-serif;padding:20px\">"
                                "<h3>Saved. Rebooting...</h3></body></html>");
  delay(1000);
  ESP.restart();
}

void portal_begin() {
  config_load();

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

  bool res = wm.autoConnect("miniTV-Setup", "minitvpass");
  if (!res) {
    mlog.println("Failed to connect, restarting");
    ESP.restart();
  }

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
  for (int i = 0; i <= m.length() && idx < MONITOR_MAX; i++) {
    if (i == m.length() || m.charAt(i) == ',') {
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
  server.on("/save", HTTP_POST, handle_save);
  server.on("/log", handle_log);
  server.begin();
  mlog.println("[PORTAL] admin UI started at http://" + WiFi.localIP().toString());
}

void portal_handle() {
  wm.process();
  server.handleClient();
}

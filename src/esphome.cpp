#include "logbuf.h"
#include "esphome.h"
#include <WiFiClient.h>
#include <ArduinoJson.h>

// Sensor endpoints exposed by the ESPHome REST API (friendly-name slugs, URL-encoded in request).
static const char* EH_IDS[EH_MAX] = {
  "IKEA Air Quality PM2.5",
  "Temperature",
  "Pressure",
  "Humidity"
};

static EspHomeState gSensors[EH_MAX];
static EhState ehState = EH_IDLE;
static unsigned long ehLast = 0;
static bool ehFirst = true;
static unsigned long ehTimer = 0;
static int ehIdx = 0;
static WiFiClient ehClient;
static String ehHost = "";
static String ehUrl = "";
static String ehBody = "";
static const unsigned long EH_INTERVAL = 30000;  // full refresh every 30s

const EspHomeState& esphome_state(int i) { return gSensors[i]; }
int esphome_count() { return EH_MAX; }

// Minimal URL-encoder (handles spaces and reserved chars for the friendly-name slugs).
static String urlencode(const String &s) {
  String out = "";
  const char *hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') ||
        ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

void esphome_begin() {
  ehState = EH_IDLE;
  ehLast = 0;
  ehFirst = true;
  ehIdx = 0;
}

void esphome_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.esphome_host[0] == 0) return;

  switch (ehState) {
    case EH_IDLE:
      if (ehFirst || millis() - ehLast >= EH_INTERVAL) {
        ehFirst = false;
        ehHost = cfg.esphome_host;
        ehUrl = String("/sensor/") + urlencode(EH_IDS[ehIdx]);
        ehBody = "";
        ehClient.stop();
        ehState = EH_CONN;
        ehTimer = millis();
      }
      break;

    case EH_CONN:
      ehClient.setTimeout(400);
      if (ehClient.connect(ehHost.c_str(), 80)) {
        ehClient.print(String("GET ") + ehUrl + " HTTP/1.0\r\n" +
                        "Host: " + ehHost + "\r\n" +
                        "User-Agent: miniTV\r\n" +
                        "Connection: close\r\n\r\n");
        ehState = EH_WAIT;
        ehTimer = millis();
      } else if (millis() - ehTimer > 4000) {
        mlog.printf("[EH] connect fail %s\n", EH_IDS[ehIdx]);
        ehState = EH_NEXT;
      }
      break;

    case EH_WAIT:
      if (ehClient.available()) { ehState = EH_READ; ehTimer = millis(); }
      else if (millis() - ehTimer > 5000) {
        mlog.printf("[EH] timeout %s\n", EH_IDS[ehIdx]);
        ehState = EH_NEXT;
      }
      break;

    case EH_READ:
      while (ehClient.available()) ehBody += (char)ehClient.read();
      if (!ehClient.connected() && !ehClient.available()) {
        ehClient.stop();
        int headerEnd = ehBody.indexOf("\r\n\r\n");
        String j = (headerEnd >= 0) ? ehBody.substring(headerEnd + 4) : ehBody;
        int b = j.indexOf('{');
        int e = j.lastIndexOf('}');
        if (b >= 0 && e > b) j = j.substring(b, e + 1);
        DynamicJsonDocument doc(512);
        EspHomeState &s = gSensors[ehIdx];
        if (j.length() && !deserializeJson(doc, j.c_str())) {
          // Display name: prefer JSON "name", else the configured friendly slug.
          const char* nm = doc["name"] | "";
          if (strlen(nm) == 0) nm = EH_IDS[ehIdx];
          strncpy(s.name, nm, sizeof(s.name) - 1);
          strncpy(s.state, doc["state"] | "", sizeof(s.state) - 1);
          strncpy(s.uom, doc["uom"] | "", sizeof(s.uom) - 1);
          sanitize_ascii(s.name);
          sanitize_ascii(s.state);
          sanitize_ascii(s.uom);
          s.valid = true;
          mlog.printf("[EH] %s = %s %s\n", s.name, s.state, s.uom);
        } else {
          s.valid = false;
          mlog.printf("[EH] parse error %s\n", EH_IDS[ehIdx]);
        }
        ehState = EH_NEXT;
      } else if (millis() - ehTimer > 6000) {
        mlog.printf("[EH] read stall %s\n", EH_IDS[ehIdx]);
        ehClient.stop();
        ehState = EH_NEXT;
      }
      break;

    case EH_NEXT:
      ehIdx++;
      if (ehIdx >= EH_MAX) {
        ehIdx = 0;
        ehLast = millis();   // full cycle done -> wait before next
      }
      ehState = EH_IDLE;
      break;
  }
}

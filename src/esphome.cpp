#include "esphome.h"
#include <WiFiClient.h>
#include <ArduinoJson.h>

// Sensor endpoints exposed by the ESPHome REST API (ordered for display).
static const char* EH_IDS[EH_MAX] = {
  "sensor-ikea_air_quality_pm2_5",
  "sensor-temperature",
  "sensor-pressure",
  "sensor-humidity"
};

static EspHomeState gSensors[EH_MAX];
static EhState ehState = EH_IDLE;
static unsigned long ehLast = 0;
static unsigned long ehTimer = 0;
static int ehIdx = 0;
static WiFiClient ehClient;
static String ehHost = "";
static String ehUrl = "";
static String ehBody = "";
static const unsigned long EH_INTERVAL = 30000;  // full refresh every 30s

const EspHomeState& esphome_state(int i) { return gSensors[i]; }
int esphome_count() { return EH_MAX; }

void esphome_begin() {
  ehState = EH_IDLE;
  ehLast = 0;
  ehIdx = 0;
}

void esphome_tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.esphome_host[0] == 0) return;

  switch (ehState) {
    case EH_IDLE:
      if (millis() - ehLast >= EH_INTERVAL) {
        ehHost = cfg.esphome_host;
        ehUrl = String("/sensor/") + EH_IDS[ehIdx];
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
        Serial.printf("[EH] connect fail %s\n", EH_IDS[ehIdx]);
        ehState = EH_NEXT;
      }
      break;

    case EH_WAIT:
      if (ehClient.available()) { ehState = EH_READ; ehTimer = millis(); }
      else if (millis() - ehTimer > 5000) {
        Serial.printf("[EH] timeout %s\n", EH_IDS[ehIdx]);
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
        Serial.printf("[EH] body[%s] len=%d: ", EH_IDS[ehIdx], j.length());
        Serial.println(j);
        DynamicJsonDocument doc(512);
        EspHomeState &s = gSensors[ehIdx];
        if (j.length() && !deserializeJson(doc, j.c_str())) {
          strncpy(s.name, doc["name"] | "", sizeof(s.name) - 1);
          strncpy(s.state, doc["state"] | "", sizeof(s.state) - 1);
          strncpy(s.uom, doc["uom"] | "", sizeof(s.uom) - 1);
          s.valid = true;
          Serial.printf("[EH] %s = %s %s\n", s.name, s.state, s.uom);
        } else {
          s.valid = false;
          Serial.printf("[EH] parse error %s\n", EH_IDS[ehIdx]);
        }
        ehState = EH_NEXT;
      } else if (millis() - ehTimer > 6000) {
        Serial.printf("[EH] read stall %s\n", EH_IDS[ehIdx]);
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

#pragma once
#include <Arduino.h>
#include <WiFiClient.h>

// Reusable non-blocking HTTP/1.0 GET state machine. One request at a time.
// Drives connect -> send -> wait -> read without ever blocking the main loop.
class HttpFsm {
 public:
  enum Status { IDLE, BUSY, DONE, FAILED };

  // Begin a GET. Returns false if a request is already in flight.
  bool begin(const String &host, const String &url, uint16_t port = 80);

  // Advance the state machine. Call every loop iteration.
  void tick();

  Status status() const { return _status; }
  bool done() const { return _status == DONE; }
  bool failed() const { return _status == FAILED; }
  bool busy() const { return _status == BUSY; }

  // Body (after HTTP headers). Valid when done(). consume() resets to IDLE.
  const String &body() const { return _body; }
  void consume() { _status = IDLE; _body = ""; _client.stop(); }

  // Timeouts (ms).
  unsigned long connectTimeout = 4000;
  unsigned long waitTimeout = 5000;
  unsigned long readTimeout = 6000;

 private:
  enum Phase { P_CONN, P_WAIT, P_READ };
  WiFiClient _client;
  Status _status = IDLE;
  Phase _phase = P_CONN;
  String _host, _url, _body;
  uint16_t _port = 80;
  unsigned long _timer = 0;

  void fail(const char *why);
};

// Strips HTTP headers; returns the JSON object substring ({...}) or "" .
String http_json_body(const String &raw);

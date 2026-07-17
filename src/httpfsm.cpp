#include "logbuf.h"
#include "httpfsm.h"
#include <ESP8266WiFi.h>

bool HttpFsm::begin(const String &host, const String &url, uint16_t port) {
  if (_status == BUSY) return false;
  if (WiFi.status() != WL_CONNECTED) { _status = FAILED; return false; }
  _host = host;
  _url = url;
  _port = port;
  _body = "";
  _body.reserve(1024);
  _client.stop();
  _status = BUSY;
  _phase = P_CONN;
  _timer = millis();
  return true;
}

void HttpFsm::fail(const char *why) {
  mlog.printf("[HTTP] %s (%s%s)\n", why, _host.c_str(), _url.c_str());
  _client.stop();
  _status = FAILED;
}

void HttpFsm::tick() {
  if (_status != BUSY) return;

  switch (_phase) {
    case P_CONN:
      if (_client.connect(_host.c_str(), _port)) {
        _client.print(String("GET ") + _url + " HTTP/1.0\r\n" +
                      "Host: " + _host + "\r\n" +
                      "User-Agent: miniDash\r\n" +
                      "Connection: close\r\n\r\n");
        _phase = P_WAIT;
        _timer = millis();
      } else if (millis() - _timer > connectTimeout) {
        fail("connect fail");
      }
      break;

    case P_WAIT:
      if (_client.available()) {
        _phase = P_READ;
        _timer = millis();
      } else if (millis() - _timer > waitTimeout) {
        fail("wait timeout");
      }
      break;

    case P_READ:
      while (_client.available()) _body += (char)_client.read();
      if (!_client.connected() && !_client.available()) {
        _client.stop();
        _status = DONE;
      } else if (millis() - _timer > readTimeout) {
        fail("read stall");
      }
      break;
  }
}

String http_json_body(const String &raw) {
  int headerEnd = raw.indexOf("\r\n\r\n");
  String j = (headerEnd >= 0) ? raw.substring(headerEnd + 4) : raw;
  int b = j.indexOf('{');
  int e = j.lastIndexOf('}');
  if (b >= 0 && e > b) return j.substring(b, e + 1);
  return String();
}

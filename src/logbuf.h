#pragma once
#include <Arduino.h>

// Tees all printed output to the hardware Serial AND a ring buffer, so the
// web portal can show a live "terminal" like ESPHome devices do.
class Log : public Print {
  size_t write(uint8_t c) override {
    Serial.write(c);
    ring[head] = (char)c;
    head = (head + 1) % LOGBUF_SIZE;
    if (head == tail) tail = (tail + 1) % LOGBUF_SIZE;
    return 1;
  }
 public:
  static const size_t LOGBUF_SIZE = 2048;
  char ring[LOGBUF_SIZE];
  int head = 0;
  int tail = 0;

  // Implement directly via write() to avoid Print's ambiguous print/println overloads.
  void print(const char* s)  { if (s) while (*s) write((uint8_t)*s++); }
  void println(const char* s) { print(s); write((uint8_t)'\n'); }
  void print(const String& s) { print(s.c_str()); }
  void println(const String& s) { println(s.c_str()); }
  void println() { write((uint8_t)'\n'); }
};

extern Log mlog;

// Returns a printable copy of the buffered log. Caller must delete[] it.
char* logbuf_copy();

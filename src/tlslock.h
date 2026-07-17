#pragma once
// A single global flag so only one BearSSL TLS session runs at a time. The
// ESP8266 heap can rarely fit two concurrent TLS clients, so the flight radar
// and moon fetchers cooperate: acquire before connecting, release when done.
bool tls_try_acquire();   // returns true and locks if free; false if busy
void tls_release();
bool tls_is_busy();

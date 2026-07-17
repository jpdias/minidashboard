#include "tlslock.h"
#include <Arduino.h>

static bool gBusy = false;
static unsigned long gAcquiredAt = 0;
static const unsigned long TLS_MAX_HOLD = 20000;  // force-release after 20s

bool tls_try_acquire() {
  // Safety: if a holder crashed or forgot to release, reclaim after a timeout.
  if (gBusy && millis() - gAcquiredAt > TLS_MAX_HOLD) gBusy = false;
  if (gBusy) return false;
  gBusy = true;
  gAcquiredAt = millis();
  return true;
}

void tls_release() { gBusy = false; }

bool tls_is_busy() { return gBusy; }

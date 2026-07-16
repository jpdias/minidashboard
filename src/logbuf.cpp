#include "logbuf.h"

Log mlog;

char* logbuf_copy() {
  int n = (mlog.head >= mlog.tail) ? (mlog.head - mlog.tail)
                                   : (mlog.LOGBUF_SIZE - mlog.tail + mlog.head);
  char* out = new char[n + 2];
  if (!out) return NULL;
  int k = 0;
  for (int i = 0; i < n; i++) {
    out[k++] = mlog.ring[(mlog.tail + i) % mlog.LOGBUF_SIZE];
  }
  out[k] = 0;
  return out;
}

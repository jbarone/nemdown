/* log.c — stderr diagnostics. */

#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void vprefixed(const char *prefix, const char *fmt, va_list ap) {
  fputs(prefix, stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

void nd_info(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vprefixed("nemdown: ", fmt, ap);
  va_end(ap);
}

void nd_warn(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vprefixed("nemdown: warning: ", fmt, ap);
  va_end(ap);
}

void nd_die(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vprefixed("nemdown: error: ", fmt, ap);
  va_end(ap);
  exit(1);
}

char *nd_strdup_fmt(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) return NULL;

  char *buf = malloc((size_t)n + 1);
  if (!buf) return NULL;

  va_start(ap, fmt);
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

#if NEMDOWN_DEBUG
void nd_dbg(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vprefixed("nemdown: debug: ", fmt, ap);
  va_end(ap);
}
#endif

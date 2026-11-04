#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static const char *base_name(const char *path)
{
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

void log_write(const char *lvl, const char *file, int line, const char *fmt, ...)
{
  int saved = errno;

  struct timespec ts;
  struct tm       tm;
  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);

  char buf[1024];
  int  n = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03ld %s %s:%d: ",
                    tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
                    lvl, base_name(file), line);
  if (n < 0)
    n = 0;

  if ((size_t)n < sizeof buf) {
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0)
      n += m;
  }
  if ((size_t)n >= sizeof buf - 1)
    n = (int)sizeof buf - 2;
  buf[n++] = '\n';

  fwrite(buf, 1, (size_t)n, stderr);
  errno = saved;
}

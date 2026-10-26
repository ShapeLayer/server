/* log.h */
#pragma once
#ifndef LOG_H
#define LOG_H

void log_write(const char *lvl, const char *file, int line, const char *fmt, ...);
#define LOG_I(...) log_write("INFO", __FILE__, __LINE__, __VA_ARGS__)
#define LOG_E(...) log_write("ERR ", __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERRNO(msg) LOG_E("%s: %s", (msg), strerror(errno))

#endif  // LOG_H

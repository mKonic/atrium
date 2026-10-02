#pragma once
// atrium's log: "00:00:01.234 [INFO] [src/file.cpp:12] message" on stderr,
// coloured on a terminal, Debug lines only with `atrium -d`.

enum class Log { Silent, Error, Info, Debug };

namespace atrium {

void log_init(Log verbosity);
bool log_enabled(Log level);
void log_write(Log level, const char* file, int line, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
// The same, with ": <strerror(errno)>" after the message.
void log_write_errno(Log level, const char* file, int line, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

} // namespace atrium

#define alog(level, fmt, ...) ::atrium::log_write(level, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)
#define alog_errno(level, fmt, ...) \
    ::atrium::log_write_errno(level, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

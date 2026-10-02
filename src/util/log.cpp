#include "util/log.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace atrium {

namespace {

Log g_verbosity = Log::Info;
timespec g_start{};
bool g_colour = false;

void vwrite(Log level, const char* file, int line, const char* fmt, va_list args, const char* suffix) {
    if (!log_enabled(level))
        return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - g_start.tv_sec) * 1000 + (now.tv_nsec - g_start.tv_nsec) / 1000000;
    const char* name = level == Log::Error ? "ERROR" : level == Log::Info ? "INFO" : "DEBUG";
    const char* colour = level == Log::Error ? "\x1b[1;31m" : level == Log::Info ? "\x1b[1;34m" : "\x1b[1;90m";
    char head[96];
    std::snprintf(head, sizeof head, "%02ld:%02ld:%02ld.%03ld ", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60,
                  ms % 1000);
    flockfile(stderr);
    std::fputs(head, stderr);
    if (g_colour)
        std::fputs(colour, stderr);
    std::fprintf(stderr, "[%s] ", name);
    if (file)
        std::fprintf(stderr, "[%s:%d] ", file, line);
    std::vfprintf(stderr, fmt, args);
    if (suffix)
        std::fprintf(stderr, ": %s", suffix);
    if (g_colour)
        std::fputs("\x1b[0m", stderr);
    std::fputc('\n', stderr);
    funlockfile(stderr);
}

} // namespace

void log_init(Log verbosity) {
    g_verbosity = verbosity;
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    g_colour = isatty(STDERR_FILENO);
}

bool log_enabled(Log level) {
    return level != Log::Silent && int(level) <= int(g_verbosity);
}

void log_write(Log level, const char* file, int line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwrite(level, file, line, fmt, args, nullptr);
    va_end(args);
}

void log_write_errno(Log level, const char* file, int line, const char* fmt, ...) {
    const int err = errno;
    va_list args;
    va_start(args, fmt);
    vwrite(level, file, line, fmt, args, std::strerror(err));
    va_end(args);
}

} // namespace atrium

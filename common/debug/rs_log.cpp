#include "rs_log.hpp"
#include "log_rotating.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
#include <time.h>
#endif

namespace upc {

namespace {

char g_process_name[32] = "?";

const char* log_process_name() {
    return g_process_name[0] ? g_process_name : "?";
}

} // namespace

void log_set_process_name(const char* name) {
    if (!name || !name[0]) {
        return;
    }
    std::strncpy(g_process_name, name, sizeof(g_process_name) - 1);
    g_process_name[sizeof(g_process_name) - 1] = '\0';
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Error: return "ERROR";
        case Level::Warn:  return "WARN";
        case Level::Info:  return "INFO";
        case Level::Debug: return "DEBUG";
        case Level::Trace: return "TRACE";
    }
    return "?";
}

void log_write(Level level, const char* src, const char* module,
               const char* fmt, ...) {
#if UPC_LOG_ENABLED
    char ts[32];
    ts[0] = '\0';

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
    struct timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        const time_t sec = now.tv_sec;
        struct tm tm_buf{};
        if (localtime_r(&sec, &tm_buf) != nullptr) {
            const size_t n = std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
            if (n > 0 && n < sizeof(ts) - 8) {
                std::snprintf(ts + n, sizeof(ts) - n, ".%06ld",
                    now.tv_nsec / 1000L);
            }
        }
    }
#endif
    if (ts[0] == '\0') {
        std::snprintf(ts, sizeof(ts), "time=?");
    }

    char msg_buf[2048];
    va_list args;
    va_start(args, fmt);
    const int msg_len = std::vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    const std::size_t est = 96U
        + (msg_len > 0 ? static_cast<std::size_t>(msg_len) : 0U) + 1U;
    log_rotating_before_write(est);

    std::fprintf(stderr, "%s level=%s proc=%s src=%s module=%s ",
        ts, level_name(level), log_process_name(),
        src ? src : "?", module ? module : "?");

    if (msg_len > 0) {
        std::fwrite(msg_buf, 1, static_cast<std::size_t>(msg_len), stderr);
    }

    std::fputc('\n', stderr);
    std::fflush(stderr);
    log_rotating_after_write();
#else
    (void)level;
    (void)src;
    (void)module;
    (void)fmt;
#endif
}

} // namespace upc

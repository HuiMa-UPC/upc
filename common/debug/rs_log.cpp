#include "rs_log.hpp"
#include "log_rotating.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
#include <time.h>
#endif

namespace upc {

namespace {

char g_process_name[32] = "?";
bool g_process_log_enabled = true;

bool parse_env_switch(const char* value, bool default_value) {
    if (!value || !value[0]) {
        return default_value;
    }
    if (std::strcmp(value, "1") == 0 ||
        std::strcmp(value, "true") == 0 ||
        std::strcmp(value, "TRUE") == 0 ||
        std::strcmp(value, "on") == 0 ||
        std::strcmp(value, "ON") == 0 ||
        std::strcmp(value, "yes") == 0 ||
        std::strcmp(value, "YES") == 0) {
        return true;
    }
    if (std::strcmp(value, "0") == 0 ||
        std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "FALSE") == 0 ||
        std::strcmp(value, "off") == 0 ||
        std::strcmp(value, "OFF") == 0 ||
        std::strcmp(value, "no") == 0 ||
        std::strcmp(value, "NO") == 0) {
        return false;
    }
    return default_value;
}

bool process_log_default_from_contract(const char* proc_name) {
    if (!proc_name || !proc_name[0]) {
        return UPC_LOG_PROC_DEFAULT != 0;
    }
    if (std::strcmp(proc_name, "rongsu_ui") == 0) {
        return UPC_LOG_PROC_RONGSU_UI != 0;
    }
    if (std::strcmp(proc_name, "rongsu_glavo") == 0) {
        return UPC_LOG_PROC_RONGSU_GLAVO != 0;
    }
    if (std::strcmp(proc_name, "rongsu_camera") == 0) {
        return UPC_LOG_PROC_RONGSU_CAMERA != 0;
    }
    if (std::strcmp(proc_name, "rongsu_broker") == 0) {
        return UPC_LOG_PROC_RONGSU_BROKER != 0;
    }
    if (std::strcmp(proc_name, "rongsu_xfr") == 0) {
        return UPC_LOG_PROC_RONGSU_XFR != 0;
    }
    if (std::strcmp(proc_name, "rongsu_visual") == 0) {
        return UPC_LOG_PROC_RONGSU_VISUAL != 0;
    }
    if (std::strcmp(proc_name, "rongsu_mqtt") == 0) {
        return UPC_LOG_PROC_RONGSU_MQTT != 0;
    }
    if (std::strcmp(proc_name, "rongsu_media") == 0) {
        return UPC_LOG_PROC_RONGSU_MEDIA != 0;
    }
    return UPC_LOG_PROC_DEFAULT != 0;
}

void refresh_process_log_switch() {
    bool default_enabled = process_log_default_from_contract(g_process_name);
    default_enabled =
        parse_env_switch(std::getenv("UPC_LOG_PROC_DEFAULT"), default_enabled);

    char proc_upper[64];
    std::size_t j = 0;
    for (std::size_t i = 0; g_process_name[i] != '\0' && j + 1 < sizeof(proc_upper); ++i) {
        const unsigned char c = static_cast<unsigned char>(g_process_name[i]);
        if (std::isalnum(c) != 0) {
            proc_upper[j++] = static_cast<char>(std::toupper(c));
        } else {
            proc_upper[j++] = '_';
        }
    }
    proc_upper[j] = '\0';

    char env_key[96];
    std::snprintf(env_key, sizeof(env_key), "UPC_LOG_PROC_%s", proc_upper);
    g_process_log_enabled =
        parse_env_switch(std::getenv(env_key), default_enabled);
}

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
    refresh_process_log_switch();
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
    if (!g_process_log_enabled) {
        return;
    }
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

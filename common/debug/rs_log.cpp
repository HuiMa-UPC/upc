#include "rs_log.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
#include <time.h>
#endif

namespace upc {

namespace {

char g_process_name[32] = "?";
bool g_process_log_enabled = true;
std::mutex g_log_mtx;

constexpr const char* kTopicLogDir = "/var/log/topic";
constexpr const char* kTopicDefault = "topic_misc.log";

bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }
    const size_t n = std::strlen(prefix);
    return std::strncmp(s, prefix, n) == 0;
}

bool contains_text(const char* s, const char* token) {
    return s && token && std::strstr(s, token) != nullptr;
}

void ensure_topic_log_dir() {
    char path[256];
    std::snprintf(path, sizeof(path), "%s", kTopicLogDir);
    const size_t len = std::strlen(path);
    if (len == 0) {
        return;
    }
    for (size_t i = 1; i < len; ++i) {
        if (path[i] != '/') {
            continue;
        }
        path[i] = '\0';
        (void)mkdir(path, 0755);
        path[i] = '/';
    }
    (void)mkdir(path, 0755);
}

const char* select_topic_file(const char* module, const char* msg) {
    if (module == nullptr) {
        return kTopicDefault;
    }

    if (std::strcmp(module, "BOOT") == 0) {
        return "topic_boot_lifecycle.log";
    }

    if (std::strcmp(module, "IPC") == 0) {
        return "topic_ipc_wire.log";
    }
    if (std::strcmp(module, "MSGBUS") == 0) {
        if (starts_with(msg, "IPC_")) {
            return "topic_ipc_message.log";
        }
        return "topic_msgbus_local.log";
    }
    if (std::strcmp(module, "MemPool") == 0 ||
        std::strcmp(module, "BufferPool") == 0) {
        return "topic_msgbus_local.log";
    }

    if (std::strcmp(module, "MqttService") == 0) {
        if (contains_text(msg, "ota")) {
            return "topic_mqtt_ota.log";
        }
        if (contains_text(msg, "telemetry") || contains_text(msg, "heartbeat")) {
            return "topic_mqtt_telemetry.log";
        }
        if (contains_text(msg, "MQTT RX frame") || contains_text(msg, "MQTT TX frame") ||
            contains_text(msg, "payload[")) {
            return "topic_mqtt_wire.log";
        }
        return "topic_mqtt_command.log";
    }

    if (std::strcmp(module, "McuLink") == 0) {
        if (starts_with(msg, "sensor:") || starts_with(msg, "env_sim:")) {
            return "topic_serial_sensor.log";
        }
        if (starts_with(msg, "laser_power_set raw=")) {
            return "topic_laser_power.log";
        }
        if (contains_text(msg, "alarm_") || contains_text(msg, "RX ALARM") ||
            contains_text(msg, "EVENT ")) {
            return "topic_serial_alarm.log";
        }
        if (contains_text(msg, "TCP_TX frame") || contains_text(msg, "tcp emergency")) {
            return "topic_tcp_motion_tx.log";
        }
        if (contains_text(msg, "tcp hb rsp") || contains_text(msg, "cmd_rate=") ||
            contains_text(msg, "data_rate=")) {
            return "topic_tcp_heartbeat.log";
        }
        if (contains_text(msg, "serial TX cmd:") || contains_text(msg, "RX LAYER") ||
            contains_text(msg, "notify_serial_event")) {
            return "topic_serial_layer.log";
        }
        return "topic_print_flow.log";
    }

    if (std::strcmp(module, "McuOta") == 0) {
        return "topic_device_ota.log";
    }

    if (std::strcmp(module, "GlavoService") == 0 ||
        std::strcmp(module, "CalParser") == 0 ||
        std::strcmp(module, "ParserContext") == 0 ||
        std::strcmp(module, "GcodeWorker") == 0 ||
        std::strcmp(module, "OvfWorker") == 0 ||
        std::strcmp(module, "ServiceBase") == 0) {
        return "topic_print_flow.log";
    }

    if (std::strcmp(module, "HostMonitor") == 0) {
        return "topic_system_resource.log";
    }

    if (std::strcmp(module, "CameraService") == 0) {
        return "topic_camera_capture.log";
    }
    if (std::strcmp(module, "CameraPreview") == 0) {
        return "topic_ui_preview.log";
    }
    if (std::strcmp(module, "VisualService") == 0) {
        return "topic_visual_firstlayer.log";
    }
    if (std::strcmp(module, "XfrService") == 0) {
        return "topic_xfr_upload.log";
    }
    if (std::strcmp(module, "AlarmState") == 0) {
        return "topic_ui_alarm_state.log";
    }

    if (std::strcmp(module, "MediaService") == 0) {
        if (contains_text(msg, "storage") || contains_text(msg, "STORRCLM")) {
            return "topic_storage_usage.log";
        }
        if (contains_text(msg, "live_rtsp")) {
            return "topic_media_rtsp.log";
        }
        return "topic_media_record.log";
    }

    return kTopicDefault;
}

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

    std::lock_guard<std::mutex> lock(g_log_mtx);
    ensure_topic_log_dir();

    const char* topic_file = select_topic_file(module, msg_buf);
    char path[320];
    std::snprintf(path, sizeof(path), "%s/%s", kTopicLogDir, topic_file);

    FILE* fp = std::fopen(path, "a");
    if (!fp) {
        fp = stderr;
    }

    std::fprintf(fp, "%s level=%s proc=%s src=%s module=%s ",
                 ts, level_name(level), log_process_name(),
                 src ? src : "?", module ? module : "?");

    if (msg_len > 0) {
        std::fwrite(msg_buf, 1, static_cast<std::size_t>(msg_len), fp);
    }

    std::fputc('\n', fp);
    std::fflush(fp);
    if (fp != stderr) {
        std::fclose(fp);
    }
#else
    (void)level;
    (void)src;
    (void)module;
    (void)fmt;
#endif
}

} // namespace upc

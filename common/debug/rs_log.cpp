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
#include <unistd.h>

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
#include <time.h>
#endif

namespace upc {

namespace {

char g_process_name[32] = "?";
bool g_process_log_enabled = true;
int g_min_level = UPC_LOG_MIN_LEVEL;
bool g_log_cfg_ready = false;
std::mutex g_log_mtx;

constexpr const char* kTopicLogDir = UPC_TOPIC_LOG_DIR;

struct TopicFp {
    FILE* fp = nullptr;
    ino_t ino = 0;
    dev_t dev = 0;
};

TopicFp g_topic_fp[static_cast<int>(LogTopic::Count)];
bool g_topic_atexit = false;

bool g_topic_dir_ready = false;

void ensure_topic_log_dir() {
    if (g_topic_dir_ready) {
        return;
    }
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
    g_topic_dir_ready = true;
}

const char* topic_file_name(LogTopic topic) {
    switch (topic) {
        case LogTopic::Boot:             return "topic_boot_lifecycle.log";
        case LogTopic::IpcWire:          return "topic_ipc_wire.log";
        case LogTopic::IpcMessage:       return "topic_ipc_message.log";
        case LogTopic::MsgbusLocal:      return "topic_msgbus_local.log";
        case LogTopic::SystemResource:   return "topic_system_resource.log";
        case LogTopic::MqttWire:         return "topic_mqtt_wire.log";
        case LogTopic::MqttState:        return "topic_mqtt_state.log";
        case LogTopic::MqttTelemetry:    return "topic_mqtt_telemetry.log";
        case LogTopic::MqttOta:          return "topic_mqtt_ota.log";
        case LogTopic::MqttCommand:      return "topic_mqtt_command.log";
        case LogTopic::XfrUpload:        return "topic_xfr_upload.log";
        case LogTopic::SerialSensor:     return "topic_serial_sensor.log";
        case LogTopic::SerialAlarm:      return "topic_serial_alarm.log";
        case LogTopic::SerialLayer:      return "topic_serial_layer.log";
        case LogTopic::TcpMotionTx:      return "topic_tcp_motion_tx.log";
        case LogTopic::TcpHeartbeat:     return "topic_tcp_heartbeat.log";
        case LogTopic::LaserPower:       return "topic_laser_power.log";
        case LogTopic::PrintFlow:        return "topic_print_flow.log";
        case LogTopic::VisualFirstlayer: return "topic_visual_firstlayer.log";
        case LogTopic::DeviceOta:        return "topic_device_ota.log";
        case LogTopic::CameraCapture:    return "topic_camera_capture.log";
        case LogTopic::MediaRecord:      return "topic_media_record.log";
        case LogTopic::MediaRtsp:        return "topic_media_rtsp.log";
        case LogTopic::StorageUsage:     return "topic_storage_usage.log";
        case LogTopic::UiPreview:        return "topic_ui_preview.log";
        case LogTopic::UiAlarmState:     return "topic_ui_alarm_state.log";
        case LogTopic::N2Ble:            return "topic_n2_ble.log";
        case LogTopic::Misc:
        case LogTopic::Count:
            break;
    }
    return "topic_misc.log";
}

int topic_index(LogTopic topic) {
    const int i = static_cast<int>(topic);
    if (i < 0 || i >= static_cast<int>(LogTopic::Count)) {
        return static_cast<int>(LogTopic::Misc);
    }
    return i;
}

void close_topic_files() {
    for (int i = 0; i < static_cast<int>(LogTopic::Count); ++i) {
        if (g_topic_fp[i].fp) {
            std::fclose(g_topic_fp[i].fp);
            g_topic_fp[i].fp = nullptr;
        }
    }
}

void register_topic_atexit() {
    if (g_topic_atexit) {
        return;
    }
    g_topic_atexit = (std::atexit(close_topic_files) == 0);
}

FILE* open_topic_fp(LogTopic topic) {
    const int idx = topic_index(topic);
    TopicFp& slot = g_topic_fp[idx];
    char path[320];
    std::snprintf(path, sizeof(path), "%s/%s", kTopicLogDir, topic_file_name(topic));

    /* rename 轮转后路径 inode 会变；copytruncate 则 inode 不变。
     * 用 path 的 stat 对比已打开 fd，过期则重开，不必每条 fclose。 */
    struct stat st_path {};
    const int path_st = ::stat(path, &st_path);
    if (slot.fp && path_st == 0 &&
        st_path.st_ino == slot.ino && st_path.st_dev == slot.dev) {
        return slot.fp;
    }
    if (slot.fp) {
        std::fclose(slot.fp);
        slot.fp = nullptr;
    }

    FILE* fp = std::fopen(path, "a");
    if (!fp) {
        g_topic_dir_ready = false;
        ensure_topic_log_dir();
        fp = std::fopen(path, "a");
    }
    if (!fp) {
        return nullptr;
    }
    struct stat st_fd {};
    if (::fstat(::fileno(fp), &st_fd) == 0) {
        slot.ino = st_fd.st_ino;
        slot.dev = st_fd.st_dev;
    } else {
        slot.ino = 0;
        slot.dev = 0;
    }
    slot.fp = fp;
    register_topic_atexit();
    return fp;
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

int parse_min_level(const char* value, int default_value) {
    if (!value || !value[0]) {
        return default_value;
    }
    if (std::strcmp(value, "0") == 0 ||
        std::strcmp(value, "error") == 0 ||
        std::strcmp(value, "ERROR") == 0) {
        return 0;
    }
    if (std::strcmp(value, "1") == 0 ||
        std::strcmp(value, "warn") == 0 ||
        std::strcmp(value, "WARN") == 0) {
        return 1;
    }
    if (std::strcmp(value, "2") == 0 ||
        std::strcmp(value, "info") == 0 ||
        std::strcmp(value, "INFO") == 0) {
        return 2;
    }
    if (std::strcmp(value, "3") == 0 ||
        std::strcmp(value, "debug") == 0 ||
        std::strcmp(value, "DEBUG") == 0) {
        return 3;
    }
    if (std::strcmp(value, "4") == 0 ||
        std::strcmp(value, "trace") == 0 ||
        std::strcmp(value, "TRACE") == 0) {
        return 4;
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

    g_min_level = parse_min_level(std::getenv("UPC_LOG_MIN_LEVEL"), UPC_LOG_MIN_LEVEL);
    if (g_min_level < 0) {
        g_min_level = 0;
    }
    if (g_min_level > 4) {
        g_min_level = 4;
    }
    g_log_cfg_ready = true;
}

void ensure_log_cfg() {
    if (!g_log_cfg_ready) {
        refresh_process_log_switch();
    }
}

const char* log_process_name() {
    return g_process_name[0] ? g_process_name : "?";
}

bool is_boot_module(const char* module) {
    return module && std::strcmp(module, "BOOT") == 0;
}

bool should_emit(Level level, const char* module) {
    if (level == Level::Error || is_boot_module(module)) {
        return true;
    }
    if (!g_process_log_enabled) {
        return false;
    }
    return static_cast<int>(level) <= g_min_level;
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

bool log_would_write(Level level, const char* module) {
#if UPC_LOG_ENABLED
    ensure_log_cfg();
    return should_emit(level, module);
#else
    (void)level;
    (void)module;
    return false;
#endif
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
               LogTopic topic, const char* fmt, ...) {
#if UPC_LOG_ENABLED
    ensure_log_cfg();
    if (!should_emit(level, module)) {
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

    /* vsnprintf 返回"本应写入"的长度：正文超过 msg_buf 时它大于缓冲区大小，
     * 直接拿去 fwrite 会读出栈上的越界数据，这里夹到实际截断后的长度。
     * 返回负值（编码错误）时缓冲区内容未定义，置空避免后续按 C 串读越界。 */
    std::size_t msg_out = 0;
    if (msg_len > 0) {
        msg_out = static_cast<std::size_t>(msg_len);
        if (msg_out > sizeof(msg_buf) - 1) {
            msg_out = sizeof(msg_buf) - 1;
        }
    } else if (msg_len < 0) {
        msg_buf[0] = '\0';
    }

    std::lock_guard<std::mutex> lock(g_log_mtx);
    ensure_topic_log_dir();

    FILE* fp = open_topic_fp(topic);
    if (!fp) {
        fp = stderr;
    }

    std::fprintf(fp, "%s level=%s proc=%s src=%s module=%s ",
                 ts, level_name(level), log_process_name(),
                 src ? src : "?", module ? module : "?");

    if (msg_out > 0) {
        std::fwrite(msg_buf, 1, msg_out, fp);
    }

    std::fputc('\n', fp);
    std::fflush(fp);
#else
    (void)level;
    (void)src;
    (void)module;
    (void)topic;
    (void)fmt;
#endif
}

} // namespace upc

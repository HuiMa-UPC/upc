#ifndef RS_LOG_HPP
#define RS_LOG_HPP

#include "upc_contract.hpp"
#include <cstdarg>

/**
 * 日志总开关 UPC_LOG_ENABLED（由 CMake 注入 0/1，默认跟随 UPC_DEBUG）。
 * UPC_LOG_ENABLED=0 时所有 UPC_LOG_* 宏为空操作，不调用 log_write。
 */
#ifndef UPC_LOG_ENABLED
#define UPC_LOG_ENABLED UPC_DEBUG
#endif

/**
 * 专题日志根目录的唯一定义点：rs_log 按专题写这里，process_ctl 也用它拼
 * boot log 路径做 stdout/stderr 重定向。别处不要再写一遍字面量，否则改路径时
 * 两边会漂移。改这里要同步 scripts/topic 里的 logrotate 路径。
 */
#define UPC_TOPIC_LOG_DIR "/var/log/topic"

namespace upc {

enum class Level : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace,
};

/**
 * 落盘专题。调用方显式指定，禁止再靠正文关键词分流。
 * 文件名与 common/COMMON.md 专题字典一一对应。
 */
enum class LogTopic : int {
    Misc = 0,
    Boot,
    IpcWire,
    IpcMessage,
    MsgbusLocal,
    SystemResource,
    MqttWire,
    MqttState,
    MqttTelemetry,
    MqttOta,
    MqttCommand,
    XfrUpload,
    SerialSensor,
    SerialAlarm,
    SerialLayer,
    TcpMotionTx,
    TcpHeartbeat,
    LaserPower,
    PrintFlow,
    VisualFirstlayer,
    DeviceOta,
    CameraCapture,
    MediaRecord,
    MediaRtsp,
    StorageUsage,
    UiPreview,
    UiAlarmState,
    N2Ble,
    Count
};

const char* level_name(Level level);

/** 进程入口调用一次，后续 UPC_LOG_* 行首带 proc=；日志按专题写入 /var/log/topic/ 下的 *.log 文件 */
void log_set_process_name(const char* name);

/** 当前配置下该级别是否会落盘（供调用方跳过昂贵的格式化） */
bool log_would_write(Level level, const char* module = nullptr);

void log_write(Level level, const char* src, const char* module,
               LogTopic topic, const char* fmt, ...)
    __attribute__((format(printf, 5, 6)));

} // namespace upc

#define UPC_LOG_MOD_(mod_on, lvl, src, module, topic, fmt, ...)                 \
    do {                                                                        \
        if ((UPC_LOG_ENABLED) && (mod_on)) {                                    \
            upc::log_write(upc::Level::lvl, src, module,                        \
                           upc::LogTopic::topic, fmt, ##__VA_ARGS__);           \
        }                                                                       \
    } while (0)

#define UPC_LOG_MSG_BUS(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MSG_BUS, lvl, "MW", "MSGBUS", MsgbusLocal, fmt, ##__VA_ARGS__)
#define UPC_LOG_MSG_BUS_TO(topic, lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MSG_BUS, lvl, "MW", "MSGBUS", topic, fmt, ##__VA_ARGS__)

#define UPC_LOG_IPC(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_IPC, lvl, "MW", "IPC", IpcWire, fmt, ##__VA_ARGS__)

#define UPC_LOG_MEM_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MEM_POOL, lvl, "MW", "MemPool", MsgbusLocal, fmt, ##__VA_ARGS__)

#define UPC_LOG_BUFFER_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_BUFFER_POOL, lvl, "MW", "BufferPool", MsgbusLocal, fmt, ##__VA_ARGS__)

#define UPC_LOG_GLAVO_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_GLAVO_SERVICE, lvl, "SVC", "GlavoService", PrintFlow, fmt, ##__VA_ARGS__)

#define UPC_LOG_MCU_LINK(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MCU_LINK, lvl, "SVC", "McuLink", PrintFlow, fmt, ##__VA_ARGS__)
#define UPC_LOG_MCU_LINK_TO(topic, lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MCU_LINK, lvl, "SVC", "McuLink", topic, fmt, ##__VA_ARGS__)

#define UPC_LOG_MCU_OTA(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MCU_OTA, lvl, "SVC", "McuOta", DeviceOta, fmt, ##__VA_ARGS__)

#define UPC_LOG_CAL_PARSER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAL_PARSER, lvl, "PARSER", "CalParser", PrintFlow, fmt, ##__VA_ARGS__)

#define UPC_LOG_PARSER_CONTEXT(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_PARSER_CONTEXT, lvl, "PARSER", "ParserContext", PrintFlow, fmt, ##__VA_ARGS__)

#define UPC_LOG_GCODE_WORKER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_GCODE_WORKER, lvl, "PARSER", "GcodeWorker", PrintFlow, fmt, ##__VA_ARGS__)

#define UPC_LOG_OVF_WORKER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_OVF_WORKER, lvl, "PARSER", "OvfWorker", PrintFlow, fmt, ##__VA_ARGS__)

#define UPC_LOG_SERVICE_BASE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_SERVICE_BASE, lvl, "SVC", "ServiceBase", PrintFlow, fmt, ##__VA_ARGS__)

#define UPC_LOG_SYS_MONITOR(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_SYS_MONITOR, lvl, "SYS", "HostMonitor", SystemResource, fmt, ##__VA_ARGS__)

#define UPC_LOG_CAMERA_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAMERA_SERVICE, lvl, "SVC", "CameraService", CameraCapture, fmt, ##__VA_ARGS__)

#define UPC_LOG_CAMERA_PREVIEW(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAMERA_PREVIEW, lvl, "UI", "CameraPreview", UiPreview, fmt, ##__VA_ARGS__)

#define UPC_LOG_XFR_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_XFR_SERVICE, lvl, "SVC", "XfrService", XfrUpload, fmt, ##__VA_ARGS__)

#define UPC_LOG_VISUAL_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_VISUAL_SERVICE, lvl, "SVC", "VisualService", VisualFirstlayer, fmt, ##__VA_ARGS__)

#define UPC_LOG_MQTT_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MQTT_SERVICE, lvl, "SVC", "MqttService", MqttCommand, fmt, ##__VA_ARGS__)
#define UPC_LOG_MQTT_SERVICE_TO(topic, lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MQTT_SERVICE, lvl, "SVC", "MqttService", topic, fmt, ##__VA_ARGS__)

#define UPC_LOG_MEDIA_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MEDIA_SERVICE, lvl, "SVC", "MediaService", MediaRecord, fmt, ##__VA_ARGS__)
#define UPC_LOG_MEDIA_SERVICE_TO(topic, lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MEDIA_SERVICE, lvl, "SVC", "MediaService", topic, fmt, ##__VA_ARGS__)

#define UPC_LOG_ALARM_STATE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_ALARM_STATE, lvl, "UI", "AlarmState", UiAlarmState, fmt, ##__VA_ARGS__)

#define UPC_LOG_N2_BLE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_N2_BLE, lvl, "SVC", "N2Ble", N2Ble, fmt, ##__VA_ARGS__)

#endif /* RS_LOG_HPP */

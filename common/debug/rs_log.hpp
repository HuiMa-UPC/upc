#ifndef RS_LOG_HPP
#define RS_LOG_HPP

#include "upc_log_modules.hpp"
#include <cstdarg>

namespace upc {

enum class Level : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace,
};

const char* level_name(Level level);

/** 进程入口调用一次，后续 UPC_LOG_* 行首带 proc= 便于 run 模式区分多进程输出 */
void log_set_process_name(const char* name);

void log_write(Level level, const char* src, const char* module,
               const char* fmt, ...) __attribute__((format(printf, 4, 5)));

} // namespace upc

#define UPC_LOG_MOD_(mod_on, lvl, src, module, fmt, ...)                        \
    do {                                                                        \
        if ((UPC_DEBUG) && (mod_on)) {                                          \
            upc::log_write(upc::Level::lvl, src, module, fmt, ##__VA_ARGS__);   \
        }                                                                       \
    } while (0)

#define UPC_LOG_MSG_BUS(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MSG_BUS, lvl, "MW", "MSGBUS", fmt, ##__VA_ARGS__)

#define UPC_LOG_IPC(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_IPC, lvl, "MW", "IPC", fmt, ##__VA_ARGS__)

#define UPC_LOG_MEM_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MEM_POOL, lvl, "MW", "MemPool", fmt, ##__VA_ARGS__)

#define UPC_LOG_BUFFER_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_BUFFER_POOL, lvl, "MW", "BufferPool", fmt, ##__VA_ARGS__)

#define UPC_LOG_THREAD_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_THREAD_POOL, lvl, "MW", "ThreadPool", fmt, ##__VA_ARGS__)

#define UPC_LOG_GLAVO_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_GLAVO_SERVICE, lvl, "SVC", "GlavoService", fmt, ##__VA_ARGS__)

#define UPC_LOG_MCU_LINK(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MCU_LINK, lvl, "SVC", "McuLink", fmt, ##__VA_ARGS__)

#define UPC_LOG_CAL_PARSER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAL_PARSER, lvl, "PARSER", "CalParser", fmt, ##__VA_ARGS__)

#define UPC_LOG_PARSER_CONTEXT(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_PARSER_CONTEXT, lvl, "PARSER", "ParserContext", fmt, ##__VA_ARGS__)

#define UPC_LOG_GCODE_WORKER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_GCODE_WORKER, lvl, "PARSER", "GcodeWorker", fmt, ##__VA_ARGS__)

#define UPC_LOG_OVF_WORKER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_OVF_WORKER, lvl, "PARSER", "OvfWorker", fmt, ##__VA_ARGS__)

#define UPC_LOG_SERVICE_BASE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_SERVICE_BASE, lvl, "SVC", "ServiceBase", fmt, ##__VA_ARGS__)

#define UPC_LOG_SYS_MONITOR(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_SYS_MONITOR, lvl, "SYS", "HostMonitor", fmt, ##__VA_ARGS__)

#define UPC_LOG_CAMERA_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAMERA_SERVICE, lvl, "SVC", "CameraService", fmt, ##__VA_ARGS__)

#define UPC_LOG_TEST_CAMERA(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_TEST_CAMERA, lvl, "TEST", "TestCamera", fmt, ##__VA_ARGS__)

#define UPC_LOG_CAMERA_PREVIEW(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAMERA_PREVIEW, lvl, "UI", "CameraPreview", fmt, ##__VA_ARGS__)

#endif /* RS_LOG_HPP */

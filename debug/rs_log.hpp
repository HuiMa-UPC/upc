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

#define UPC_LOG_MEM_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_MEM_POOL, lvl, "MW", "MemPool", fmt, ##__VA_ARGS__)

#define UPC_LOG_BUFFER_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_BUFFER_POOL, lvl, "MW", "BufferPool", fmt, ##__VA_ARGS__)

#define UPC_LOG_THREAD_POOL(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_THREAD_POOL, lvl, "MW", "ThreadPool", fmt, ##__VA_ARGS__)

#define UPC_LOG_GLAVO_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_GLAVO_SERVICE, lvl, "SVC", "GlavoService", fmt, ##__VA_ARGS__)

#define UPC_LOG_TCP_SENDER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_TCP_SENDER, lvl, "SVC", "TcpSender", fmt, ##__VA_ARGS__)

#define UPC_LOG_CAL_PARSER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_CAL_PARSER, lvl, "SVC", "CalParser", fmt, ##__VA_ARGS__)

#define UPC_LOG_PARSER_CONTEXT(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_PARSER_CONTEXT, lvl, "SVC", "ParserContext", fmt, ##__VA_ARGS__)

#define UPC_LOG_GCODE_WORKER(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_GCODE_WORKER, lvl, "SVC", "GcodeWorker", fmt, ##__VA_ARGS__)

#define UPC_LOG_SERVICE_BASE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_SERVICE_BASE, lvl, "SVC", "ServiceBase", fmt, ##__VA_ARGS__)

#define UPC_LOG_SYS_MONITOR(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_SYS_MONITOR, lvl, "SYS", "HostMonitor", fmt, ##__VA_ARGS__)

#endif /* RS_LOG_HPP */

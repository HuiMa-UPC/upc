#ifndef MW_DEBUG_HPP
#define MW_DEBUG_HPP

#include "msg_types.hpp"
#include <cstddef>
#include <cstdio>

// ============================================================================
// 总开关：cmake -DMIDDLEWARE_DEBUG=ON  或 编译前 #define MIDDLEWARE_DEBUG 1
// 关闭时所有 MW_LOG_* 宏展开为空，零运行时开销
// ============================================================================
#ifndef MIDDLEWARE_DEBUG
#define MIDDLEWARE_DEBUG 0
#endif

#if MIDDLEWARE_DEBUG

#ifndef MW_DEBUG_MSGBUS
#define MW_DEBUG_MSGBUS 1
#endif
#ifndef MW_DEBUG_MEMPOOL
#define MW_DEBUG_MEMPOOL 0
#endif
#ifndef MW_DEBUG_BUFFERPOOL
#define MW_DEBUG_BUFFERPOOL 0
#endif
#ifndef MW_DEBUG_THREADPOOL
#define MW_DEBUG_THREADPOOL 1
#endif

#define MW_LOG_IMPL(tag, fmt, ...) \
    std::fprintf(stderr, "[MW|%s] " fmt "\n", tag, ##__VA_ARGS__)

#if MW_DEBUG_MSGBUS
#define MW_LOG_MSGBUS(fmt, ...) MW_LOG_IMPL("MSGBUS", fmt, ##__VA_ARGS__)
#else
#define MW_LOG_MSGBUS(fmt, ...) ((void)0)
#endif

#if MW_DEBUG_MEMPOOL
#define MW_LOG_MEMPOOL(fmt, ...) MW_LOG_IMPL("MEM", fmt, ##__VA_ARGS__)
#else
#define MW_LOG_MEMPOOL(fmt, ...) ((void)0)
#endif

#if MW_DEBUG_BUFFERPOOL
#define MW_LOG_BUFFERPOOL(fmt, ...) MW_LOG_IMPL("BUF", fmt, ##__VA_ARGS__)
#else
#define MW_LOG_BUFFERPOOL(fmt, ...) ((void)0)
#endif

#if MW_DEBUG_THREADPOOL
#define MW_LOG_THREADPOOL(fmt, ...) MW_LOG_IMPL("POOL", fmt, ##__VA_ARGS__)
#else
#define MW_LOG_THREADPOOL(fmt, ...) ((void)0)
#endif

#else // !MIDDLEWARE_DEBUG

#define MW_LOG_MSGBUS(fmt, ...) ((void)0)
#define MW_LOG_MEMPOOL(fmt, ...) ((void)0)
#define MW_LOG_BUFFERPOOL(fmt, ...) ((void)0)
#define MW_LOG_THREADPOOL(fmt, ...) ((void)0)

#endif // MIDDLEWARE_DEBUG

namespace middleware {

class ThreadPool;

const char* mw_debug_module_name(ModuleId id);
const char* mw_debug_msgtype_name(MsgType type);
const char* mw_debug_payload_summary(const MsgPayload& payload, char* buf, size_t buf_size);

#if MIDDLEWARE_DEBUG
void mw_debug_log_message(const char* phase, const Message& msg,
    size_t handler_count = 0, size_t queue_depth = 0);
void mw_debug_dump_status();
void mw_debug_dump_thread_pool(const ThreadPool* pool);
#else
inline void mw_debug_log_message(const char*, const Message&, size_t = 0, size_t = 0) {}
inline void mw_debug_dump_status() {}
inline void mw_debug_dump_thread_pool(const ThreadPool*) {}
#endif

} // namespace middleware

#endif

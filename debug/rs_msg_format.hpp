#ifndef RS_MSG_FORMAT_HPP
#define RS_MSG_FORMAT_HPP

#include "upc_log_modules.hpp"
#include "msg_types.hpp"
#include <cstddef>

namespace middleware {

class ThreadPool;

const char* upc_msg_module_name(ModuleId id);
const char* upc_msg_type_name(MsgType type);
const char* upc_msg_payload_summary(const MsgPayload& payload, char* buf, size_t buf_size);

#if UPC_DEBUG && UPC_MOD_MSG_BUS
void upc_msg_log_message(const char* phase, const Message& msg,
    size_t handler_count = 0, size_t queue_depth = 0);
void upc_msg_dump_status();
void upc_msg_dump_thread_pool(const ThreadPool* pool);
#else
inline void upc_msg_log_message(const char*, const Message&, size_t = 0, size_t = 0) {}
inline void upc_msg_dump_status() {}
inline void upc_msg_dump_thread_pool(const ThreadPool*) {}
#endif

} // namespace middleware

#endif /* RS_MSG_FORMAT_HPP */

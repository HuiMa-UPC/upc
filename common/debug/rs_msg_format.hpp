#ifndef RS_MSG_FORMAT_HPP
#define RS_MSG_FORMAT_HPP

#include "upc_contract.hpp"
#include "msg_runtime.hpp"
#include <cstddef>

namespace middleware {

const char* upc_msg_module_name(ModuleId id);
const char* upc_msg_type_name(MsgType type);
const char* upc_msg_payload_summary(const MsgPayload& payload, char* buf, size_t buf_size);

#if UPC_DEBUG && (UPC_MOD_MSG_BUS || UPC_MOD_IPC)
void upc_msg_log_message(const char* phase, const Message& msg,
    size_t handler_count = 0, size_t queue_depth = 0);
void upc_msg_dump_status();
#else
inline void upc_msg_log_message(const char*, const Message&, size_t = 0, size_t = 0) {}
inline void upc_msg_dump_status() {}
#endif

} // namespace middleware

#endif /* RS_MSG_FORMAT_HPP */

#include "rs_msg_format.hpp"
#include "rs_log.hpp"

#if UPC_DEBUG

#include <cctype>
#include <cstring>

#if UPC_MOD_MSG_BUS || UPC_MOD_IPC
#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"
#endif

namespace middleware {

namespace {

bool is_mostly_text(const uint8_t* data, size_t len) {
    if(!data || len == 0) {
        return false;
    }
    size_t printable = 0;
    for(size_t i = 0; i < len; ++i) {
        const unsigned char c = data[i];
        if(c == '\n' || c == '\r' || c == '\t' || std::isprint(c)) {
            ++printable;
        }
    }
    return printable * 2 >= len;
}

void append_truncated_text(char* buf, size_t buf_size, const uint8_t* data, size_t len) {
    if(!buf || buf_size == 0) {
        return;
    }
    size_t n = len;
    if(n > 48) {
        n = 48;
    }
    size_t pos = std::strlen(buf);
    if(pos + 2 >= buf_size) {
        return;
    }
    buf[pos++] = '"';
    for(size_t i = 0; i < n && pos + 2 < buf_size; ++i) {
        const char c = static_cast<char>(data[i]);
        if(c == '\n' || c == '\r') {
            buf[pos++] = ' ';
        } else if(std::isprint(static_cast<unsigned char>(c))) {
            buf[pos++] = c;
        } else {
            buf[pos++] = '.';
        }
    }
    if(len > n && pos + 4 < buf_size) {
        std::strcpy(buf + pos, "...\"");
    } else if(pos + 1 < buf_size) {
        buf[pos++] = '"';
        buf[pos] = '\0';
    }
}

} // namespace

const char* upc_msg_module_name(ModuleId id) {
    switch(id) {
    case ModuleId::UI:         return "UI";
    case ModuleId::APP_GLAVO:  return "APP_GLAVO";
    case ModuleId::APP_CAMERA: return "APP_CAMERA";
    default:                   return "?";
    }
}

const char* upc_msg_type_name(MsgType type) {
    switch(type) {
    case MsgType::APP_Update_Ntf: return "APP_Update_Ntf";
    case MsgType::UI_PM25_Req: return "UI_PM25_Req";
    case MsgType::APP_PM25_Rsp: return "APP_PM25_Rsp";
    case MsgType::UI_CLOSE_DOOR_Req: return "UI_CLOSE_DOOR_Req";
    case MsgType::APP_CLOSE_DOOR_Rsp: return "APP_CLOSE_DOOR_Rsp";
    case MsgType::Fs_DirOpen_Req: return "Fs_DirOpen_Req";
    case MsgType::Fs_DirOpen_Rsp: return "Fs_DirOpen_Rsp";
    case MsgType::Fs_Delete_Ntf: return "Fs_Delete_Ntf";
    case MsgType::UI_OpenDoor_Req: return "UI_OpenDoor_Req";
    case MsgType::APP_OpenDoor_Rsp: return "APP_OpenDoor_Rsp";
    case MsgType::UI_Purify_Req: return "UI_Purify_Req";
    case MsgType::APP_Purify_Progress: return "APP_Purify_Progress";
    case MsgType::APP_Purify_Rsp: return "APP_Purify_Rsp";
    case MsgType::UI_Print_Prepare_Req: return "UI_Print_Prepare_Req";
    case MsgType::APP_Print_Prepare_Rsp: return "APP_Print_Prepare_Rsp";
    case MsgType::UI_Parser_Exit_Ntf: return "UI_Parser_Exit_Ntf";
    case MsgType::UI_Print_Start_Req: return "UI_Print_Start_Req";
    case MsgType::APP_Print_Progress_Ntf: return "APP_Print_Progress_Ntf";
    case MsgType::UI_Print_Terminate_Req: return "UI_Print_Terminate_Req";
    case MsgType::APP_Print_Terminate_Done: return "APP_Print_Terminate_Done";
    case MsgType::UI_Supply_Status_Req: return "UI_Supply_Status_Req";
    case MsgType::APP_Supply_Status_Rsp: return "APP_Supply_Status_Rsp";
    case MsgType::UI_Camera_Stream_Start_Req: return "UI_Camera_Stream_Start_Req";
    case MsgType::APP_Camera_Stream_Start_Rsp: return "APP_Camera_Stream_Start_Rsp";
    case MsgType::UI_Camera_Stream_Stop_Req: return "UI_Camera_Stream_Stop_Req";
    case MsgType::APP_Camera_Stream_Stop_Done: return "APP_Camera_Stream_Stop_Done";
    case MsgType::APP_Camera_Frame_Ntf: return "APP_Camera_Frame_Ntf";
    case MsgType::APP_Print_Video_Session_Start_Ntf:
        return "APP_Print_Video_Session_Start_Ntf";
    case MsgType::APP_Print_Layer_Snap_Ntf: return "APP_Print_Layer_Snap_Ntf";
    case MsgType::APP_Print_Video_Session_Finish_Ntf:
        return "APP_Print_Video_Session_Finish_Ntf";
    case MsgType::APP_Print_Video_Session_Abort_Ntf:
        return "APP_Print_Video_Session_Abort_Ntf";
    case MsgType::UI_Mcu_Ota_Start_Req: return "UI_Mcu_Ota_Start_Req";
    case MsgType::UI_Mcu_Ota_Cancel_Ntf: return "UI_Mcu_Ota_Cancel_Ntf";
    case MsgType::APP_Mcu_Ota_Status_Ntf: return "APP_Mcu_Ota_Status_Ntf";
    default: return "Msg";
    }
}

const char* upc_msg_payload_summary(const MsgPayload& payload, char* buf, size_t buf_size) {
    if(!buf || buf_size == 0) {
        return "";
    }
    buf[0] = '\0';

    switch(payload.type) {
    case MsgPayload::Type::Empty:
        std::snprintf(buf, buf_size, "empty");
        break;
    case MsgPayload::Type::Inline:
        if(payload.length == 0) {
            std::snprintf(buf, buf_size, "inline(0)");
        } else if(is_mostly_text(payload.data.bytes.data(), payload.length)) {
            std::snprintf(buf, buf_size, "inline(%zu)=", payload.length);
            append_truncated_text(buf, buf_size, payload.data.bytes.data(), payload.length);
        } else {
            std::snprintf(buf, buf_size, "inline(%zu,bin)", payload.length);
        }
        if(payload.data.value != 0) {
            const size_t pos = std::strlen(buf);
            std::snprintf(buf + pos, buf_size - pos, " val=%u", payload.data.value);
        }
        break;
    case MsgPayload::Type::BufferRef:
        std::snprintf(buf, buf_size, "buf(id=%u off=%u len=%u)",
                      payload.data.buffer_ref.buf_id,
                      payload.data.buffer_ref.offset,
                      payload.data.buffer_ref.length);
        break;
    case MsgPayload::Type::Value:
        std::snprintf(buf, buf_size, "value=%u", payload.data.value);
        break;
    }
    return buf;
}

#if UPC_MOD_MSG_BUS || UPC_MOD_IPC

void upc_msg_log_message(const char* phase, const Message& msg,
                         size_t handler_count, size_t queue_depth) {
    char payload[128];
    upc_msg_payload_summary(msg.payload, payload, sizeof(payload));

    if(handler_count > 0) {
        UPC_LOG_MSG_BUS(Debug,
                        "%s %s %s->%s pri=%u handlers=%zu payload={%s}",
                        phase,
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id),
                        upc_msg_module_name(msg.target_id),
                        static_cast<unsigned>(msg.priority),
                        handler_count,
                        payload);
    } else {
        UPC_LOG_MSG_BUS(Debug,
                        "%s %s %s->%s pri=%u queue=%zu payload={%s}",
                        phase,
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id),
                        upc_msg_module_name(msg.target_id),
                        static_cast<unsigned>(msg.priority),
                        queue_depth,
                        payload);
    }
}

void upc_msg_dump_status() {
    UPC_LOG_MSG_BUS(Info, "status pending=%zu full=%s subs=%zu",
                    MsgBus::instance().pending_count(),
                    MsgBus::instance().is_full() ? "yes" : "no",
                    MsgBus::instance().subscriber_count());

#if UPC_MOD_MEM_POOL
    const auto mem_stats = MemPool::instance().get_stats();
    for(const auto& s : mem_stats) {
        if(s.total_blocks == 0) {
            continue;
        }
        UPC_LOG_MEM_POOL(Debug, "class %zuB: used=%zu/%zu free=%zu",
                         s.block_size, s.used_blocks, s.total_blocks, s.free_blocks);
    }
#endif

#if UPC_MOD_BUFFER_POOL
    const auto buf_stats = BufferPool::instance().get_stats();
    UPC_LOG_BUFFER_POOL(Debug, "buffers used=%zu/%zu bytes=%zu",
                        buf_stats.used_count, buf_stats.capacity, buf_stats.bytes_used);
#endif
}

#endif /* UPC_MOD_MSG_BUS || UPC_MOD_IPC */

} // namespace middleware

#endif /* UPC_DEBUG */

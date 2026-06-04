#include "mw_debug.hpp"

#if MIDDLEWARE_DEBUG

#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"
#include "thread_pool.hpp"

#include <cctype>
#include <cstring>

namespace middleware {

namespace {

bool is_mostly_text(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return false;
    }
    size_t printable = 0;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = data[i];
        if (c == '\n' || c == '\r' || c == '\t' || std::isprint(c)) {
            ++printable;
        }
    }
    return printable * 2 >= len;
}

void append_truncated_text(char* buf, size_t buf_size, const uint8_t* data, size_t len) {
    if (!buf || buf_size == 0) {
        return;
    }
    size_t n = len;
    if (n > 48) {
        n = 48;
    }
    size_t pos = std::strlen(buf);
    if (pos + 2 >= buf_size) {
        return;
    }
    buf[pos++] = '"';
    for (size_t i = 0; i < n && pos + 2 < buf_size; ++i) {
        const char c = static_cast<char>(data[i]);
        if (c == '\n' || c == '\r') {
            buf[pos++] = ' ';
        } else if (std::isprint(static_cast<unsigned char>(c))) {
            buf[pos++] = c;
        } else {
            buf[pos++] = '.';
        }
    }
    if (len > n && pos + 4 < buf_size) {
        std::strcpy(buf + pos, "...\"");
    } else if (pos + 1 < buf_size) {
        buf[pos++] = '"';
        buf[pos] = '\0';
    }
}

} // namespace

const char* mw_debug_module_name(ModuleId id) {
    switch (id) {
        case ModuleId::UI: return "UI";
        case ModuleId::APP_GLAVO: return "APP_GLAVO";
        default: return "?";
    }
}

const char* mw_debug_msgtype_name(MsgType type) {
    switch (type) {
        case MsgType::APP_Update_Ntf: return "APP_Update_Ntf";
        case MsgType::UI_PM25_Req: return "UI_PM25_Req";
        case MsgType::APP_PM25_Rsp: return "APP_PM25_Rsp";
        case MsgType::UI_CLOSE_DOOR_Req: return "UI_CLOSE_DOOR_Req";
        case MsgType::APP_CLOSE_DOOR_Rsp: return "APP_CLOSE_DOOR_Rsp";
        case MsgType::Fs_DirOpen_Req: return "Fs_DirOpen_Req";
        case MsgType::Fs_DirOpen_Rsp: return "Fs_DirOpen_Rsp";
    

        default: return "Msg";
    }
}

const char* mw_debug_payload_summary(const MsgPayload& payload, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) {
        return "";
    }
    buf[0] = '\0';

    switch (payload.type) {
        case MsgPayload::Type::Empty:
            std::snprintf(buf, buf_size, "empty");
            break;
        case MsgPayload::Type::Inline:
            if (payload.length == 0) {
                std::snprintf(buf, buf_size, "inline(0)");
            } else if (is_mostly_text(payload.data.bytes.data(), payload.length)) {
                std::snprintf(buf, buf_size, "inline(%zu)=", payload.length);
                append_truncated_text(buf, buf_size, payload.data.bytes.data(), payload.length);
            } else {
                std::snprintf(buf, buf_size, "inline(%zu,bin)", payload.length);
            }
            if (payload.data.value != 0) {
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

void mw_debug_log_message(const char* phase, const Message& msg,
    size_t handler_count, size_t queue_depth) {
    char payload[128];
    mw_debug_payload_summary(msg.payload, payload, sizeof(payload));

    if (handler_count > 0) {
        MW_LOG_MSGBUS("%s %s %s->%s pri=%u handlers=%zu payload={%s}",
            phase,
            mw_debug_msgtype_name(msg.type),
            mw_debug_module_name(msg.sender_id),
            mw_debug_module_name(msg.target_id),
            static_cast<unsigned>(msg.priority),
            handler_count,
            payload);
    } else {
        MW_LOG_MSGBUS("%s %s %s->%s pri=%u queue=%zu payload={%s}",
            phase,
            mw_debug_msgtype_name(msg.type),
            mw_debug_module_name(msg.sender_id),
            mw_debug_module_name(msg.target_id),
            static_cast<unsigned>(msg.priority),
            queue_depth,
            payload);
    }
}

void mw_debug_dump_status() {
    MW_LOG_MSGBUS("status pending=%zu full=%s subs=%zu",
        MsgBus::instance().pending_count(),
        MsgBus::instance().is_full() ? "yes" : "no",
        MsgBus::instance().subscriber_count());

#if MW_DEBUG_MEMPOOL
    const auto mem_stats = MemPool::instance().get_stats();
    for (const auto& s : mem_stats) {
        if (s.total_blocks == 0) {
            continue;
        }
        MW_LOG_MEMPOOL("class %zuB: used=%zu/%zu free=%zu",
            s.block_size, s.used_blocks, s.total_blocks, s.free_blocks);
    }
#endif

#if MW_DEBUG_BUFFERPOOL
    const auto buf_stats = BufferPool::instance().get_stats();
    MW_LOG_BUFFERPOOL("buffers used=%zu/%zu bytes=%zu",
        buf_stats.used_count, buf_stats.capacity, buf_stats.bytes_used);
#endif
}

void mw_debug_dump_thread_pool(const ThreadPool* pool) {
    if (!pool) {
        MW_LOG_THREADPOOL("status (null pool)");
        return;
    }
    MW_LOG_THREADPOOL("status threads=%zu active=%zu queued=%zu",
        pool->thread_count(), pool->active_count(), pool->queue_depth());
}

} // namespace middleware

#else

namespace middleware {
} // namespace middleware

#endif // MIDDLEWARE_DEBUG

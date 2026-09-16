#include "ipc_wire.hpp"
#include "buffer_pool.hpp"

#include <cstring>

namespace upc_ipc {

namespace {

middleware::MsgPayload payload_from_wire(WirePayloadType wt,
                                         const uint8_t* bytes,
                                         uint32_t len) {
    switch (wt) {
        case WirePayloadType::Empty:
            return {};
        case WirePayloadType::Value:
            if (len < sizeof(uint32_t)) {
                return {};
            }
            {
                uint32_t v = 0;
                std::memcpy(&v, bytes, sizeof(v));
                return middleware::MsgPayload::from_value(v);
            }
        case WirePayloadType::Inline:
            return len > 0 ? middleware::MsgPayload::from_bytes(bytes, len) : middleware::MsgPayload{};
        default:
            return {};
    }
}

WirePayloadType payload_to_wire(const middleware::MsgPayload& p) {
    switch (p.type) {
        case middleware::MsgPayload::Type::Empty:
            return WirePayloadType::Empty;
        case middleware::MsgPayload::Type::Inline:
            return WirePayloadType::Inline;
        case middleware::MsgPayload::Type::Value:
            return WirePayloadType::Value;
        case middleware::MsgPayload::Type::BufferRef:
            return WirePayloadType::Inline;
    }
    return WirePayloadType::Empty;
}

bool gather_payload_bytes(const middleware::MsgPayload& p,
                          std::vector<uint8_t>& bytes) {
    switch (p.type) {
        case middleware::MsgPayload::Type::Empty:
            return true;
        case middleware::MsgPayload::Type::Inline:
            bytes.assign(p.data.bytes.begin(), p.data.bytes.begin() + p.length);
            return true;
        case middleware::MsgPayload::Type::Value:
            bytes.resize(sizeof(uint32_t));
            std::memcpy(bytes.data(), &p.data.value, sizeof(uint32_t));
            return true;
        case middleware::MsgPayload::Type::BufferRef: {
            const auto& ref = p.data.buffer_ref;
            const uint8_t* ptr =
                middleware::BufferPool::instance().get_ptr(ref.buf_id);
            if (!ptr) {
                return false;
            }
            bytes.assign(ptr + ref.offset, ptr + ref.offset + ref.length);
            return true;
        }
    }
    return false;
}

} // namespace

bool wire_encode(const middleware::Message& msg, std::vector<uint8_t>& out) {
    std::vector<uint8_t> body;
    if (!gather_payload_bytes(msg.payload, body)) {
        return false;
    }
    if (body.size() > WIRE_MAX_PAYLOAD) {
        return false;
    }

    WireHeader hdr{};
    hdr.magic          = WIRE_MAGIC;
    hdr.version        = WIRE_VERSION;
    hdr.type           = static_cast<uint32_t>(msg.type);
    hdr.priority       = static_cast<uint8_t>(msg.priority);
    hdr.sender_id      = static_cast<uint32_t>(msg.sender_id);
    hdr.target_id      = static_cast<uint32_t>(msg.target_id);
    hdr.timestamp      = msg.timestamp;
    hdr.payload_type   = static_cast<uint8_t>(payload_to_wire(msg.payload));
    hdr.payload_length = static_cast<uint32_t>(body.size());

    out.resize(sizeof(WireHeader) + body.size());
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    if (!body.empty()) {
        std::memcpy(out.data() + sizeof(WireHeader), body.data(), body.size());
    }
    return true;
}

bool wire_decode(const uint8_t* data, size_t len, middleware::Message& out) {
    if (len < sizeof(WireHeader)) {
        return false;
    }

    WireHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != WIRE_MAGIC || hdr.version != WIRE_VERSION) {
        return false;
    }
    if (hdr.payload_length > WIRE_MAX_PAYLOAD) {
        return false;
    }
    if (sizeof(WireHeader) + hdr.payload_length != len) {
        return false;
    }

    const uint8_t* payload =
        hdr.payload_length > 0 ? data + sizeof(WireHeader) : nullptr;

    out.type      = static_cast<middleware::MsgType>(hdr.type);
    out.priority  = static_cast<middleware::MsgPriority>(hdr.priority);
    out.sender_id = static_cast<middleware::ModuleId>(hdr.sender_id);
    out.target_id = static_cast<middleware::ModuleId>(hdr.target_id);
    out.timestamp = hdr.timestamp;
    out.payload   = payload_from_wire(
        static_cast<WirePayloadType>(hdr.payload_type),
        payload,
        hdr.payload_length);

    /* 线上说有 payload，解出来却是空——只可能是 BufferPool 耗尽导致
     * from_bytes 退化成 Empty。此时必须报解码失败：否则订阅者会拿到一条
     * "字段齐全但内容为空"的消息（比如层进度、切片数据）当成有效值用，
     * 变成无声的数据丢失，且日志里一行痕迹都没有。 */
    if (static_cast<WirePayloadType>(hdr.payload_type) == WirePayloadType::Inline &&
        hdr.payload_length > 0 &&
        out.payload.type == middleware::MsgPayload::Type::Empty) {
        return false;
    }
    return true;
}

bool wire_peek_route(const uint8_t* data, size_t len,
                     middleware::ModuleId& sender_id,
                     middleware::ModuleId& target_id)
{
    if (len < sizeof(WireHeader)) {
        return false;
    }

    WireHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != WIRE_MAGIC || hdr.version != WIRE_VERSION) {
        return false;
    }
    if (hdr.payload_length > WIRE_MAX_PAYLOAD) {
        return false;
    }
    if (sizeof(WireHeader) + hdr.payload_length != len) {
        return false;
    }

    sender_id = static_cast<middleware::ModuleId>(hdr.sender_id);
    target_id = static_cast<middleware::ModuleId>(hdr.target_id);
    return true;
}

} // namespace upc_ipc

#ifndef MSG_RUNTIME_HPP
#define MSG_RUNTIME_HPP

/**
 * @file msg_runtime.hpp
 * @brief 进程内消息运行时类型（MsgPayload / Message，依赖 BufferPool）
 *
 * 跨进程 ABI 契约（ModuleId / MsgType / POD Payload）见 common/upc_contract.hpp。
 */

#include "upc_contract.hpp"
#include "buffer_pool.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace middleware {

struct MsgPayload {
    static constexpr size_t INLINE_SIZE = 256;

    enum class Type : uint8_t {
        Empty,
        Inline,
        BufferRef,
        Value,
    };

    Type type = Type::Empty;

    union {
        std::array<uint8_t, INLINE_SIZE> bytes;
        struct {
            uint32_t buf_id;
            uint32_t offset;
            uint32_t length;
        } buffer_ref;
        uint32_t value;
    } data{};

    size_t length = 0;

    MsgPayload() = default;
    ~MsgPayload();
    MsgPayload(const MsgPayload& other);
    MsgPayload& operator=(const MsgPayload& other);
    MsgPayload(MsgPayload&& other) noexcept;
    MsgPayload& operator=(MsgPayload&& other) noexcept;

    static MsgPayload from_bytes(const uint8_t* src, size_t len) {
        MsgPayload p;
        if (!src || len == 0) {
            return p;
        }

        if (len <= INLINE_SIZE) {
            p.type = Type::Inline;
            p.length = len;
            std::memcpy(p.data.bytes.data(), src, len);
            return p;
        }

        const BufferId id = BufferPool::instance().allocate(len);
        if (id != INVALID_BUFFER_ID) {
            uint8_t* ptr = BufferPool::instance().get_ptr(id);
            if (ptr) {
                std::memcpy(ptr, src, len);
                return from_buffer(id, 0, static_cast<uint32_t>(len));
            }
            BufferPool::instance().release(id);
        }

        /* 大 payload 分配失败：返回 Empty，禁止截断为 Inline */
        return p;
    }

    static MsgPayload from_buffer(uint32_t buf_id, uint32_t offset, uint32_t len) {
        MsgPayload p;
        p.type = Type::BufferRef;
        p.data.buffer_ref = {buf_id, offset, len};
        p.length = len;
        return p;
    }

    static MsgPayload from_value(uint32_t v) {
        MsgPayload p;
        p.type = Type::Value;
        p.data.value = v;
        p.length = sizeof(v);
        return p;
    }
};

// MsgPayload RAII 实现（利用 BufferPool 引用计数）
inline MsgPayload::~MsgPayload() {
    if (type == Type::BufferRef) {
        BufferPool::instance().release(data.buffer_ref.buf_id);
    }
}

inline MsgPayload::MsgPayload(const MsgPayload& other) : type(other.type), length(other.length) {
    data = other.data;
    if (type == Type::BufferRef) {
        BufferPool::instance().acquire(data.buffer_ref.buf_id);
    }
}

inline MsgPayload& MsgPayload::operator=(const MsgPayload& other) {
    if (this != &other) {
        if (type == Type::BufferRef) {
            BufferPool::instance().release(data.buffer_ref.buf_id);
        }
        type = other.type;
        length = other.length;
        if (type == Type::BufferRef) {
            BufferPool::instance().acquire(other.data.buffer_ref.buf_id);
        }
        data = other.data;
    }
    return *this;
}

inline MsgPayload::MsgPayload(MsgPayload&& other) noexcept
    : type(other.type), data(other.data), length(other.length) {
    other.type = Type::Empty;
    other.length = 0;
}

inline MsgPayload& MsgPayload::operator=(MsgPayload&& other) noexcept {
    if (this != &other) {
        if (type == Type::BufferRef) {
            BufferPool::instance().release(data.buffer_ref.buf_id);
        }
        type = other.type;
        data = other.data;
        length = other.length;
        other.type = Type::Empty;
        other.length = 0;
    }
    return *this;
}

struct Message {
    MsgType      type;
    MsgPriority  priority = MsgPriority::Normal;
    ModuleId     sender_id;
    ModuleId     target_id = ModuleId{0};
    uint32_t     timestamp = 0;
    MsgPayload   payload;

    Message() = default;

    Message(MsgType t, ModuleId sender, ModuleId target = ModuleId{0})
        : type(t), sender_id(sender), target_id(target) {}

    Message(const Message&) = default;
    Message& operator=(const Message&) = default;
    Message(Message&&) = default;
    Message& operator=(Message&&) = default;
    ~Message() = default;
};

} // namespace middleware

#endif /* MSG_RUNTIME_HPP */

#ifndef MSG_TYPES_HPP
#define MSG_TYPES_HPP

#include "buffer_pool.hpp"
#include <cstdint>
#include <cstring>
#include <array>

namespace middleware {

enum class ModuleId : uint32_t {
    UI        = 0x01,
    APP_MCU,    /**< MCU 板后端服务 */
    APP_GLAVO,  /**< 振镜控制板后端服务 */
};

enum class MsgType : uint32_t {
    APP_Update_Ntf      = 0x1000,  /**< MCU → UI 环境/状态推送 */
    UI_PM25_Req,  /**< UI → MCU PM2.5 检测请求 */
    APP_PM25_Rsp ,  /**< MCU → UI PM2.5 检测响应 */
    UI_CLOSE_DOOR_Req,  /**< UI → MCU 关门检测请求 */
    APP_CLOSE_DOOR_Rsp,  /**< MCU → UI 关门检测响应 */
    Fs_DirOpen_Req,  /**< UI → GLAVO 打开目录（无 body） */
    Fs_DirOpen_Rsp,  /**< GLAVO → UI 目录+文件列表（BufferRef：FsDirHeader + N×FileEntryPayload） */

    // ---- 打印流程：MCU / GLAVO 作业 ----
    UI_OpenDoor_Req,  /**< UI → MCU 开门动作请求 */
    APP_OpenDoor_Rsp,  /**< MCU → UI 开门完成 */
    UI_Purify_Req,  /**< UI → MCU 启动净化 */
    APP_Purify_Progress,  /**< MCU → UI 净化进度 */
    APP_Purify_Rsp,  /**< MCU → UI 净化完成 */
    UI_Print_Start_Req,  /**< UI → GLAVO 按 file_index 启动打印作业（PrintStartReqPayload） */
    APP_Print_Progress_Ntf,  /**< GLAVO → UI 打印进度推送 */
    UI_Print_Terminate_Req,  /**< UI → GLAVO 终止打印作业 */
};

enum class MsgPriority : uint8_t {
    Low       = 0,
    Normal    = 1,
    High      = 2,
    Realtime  = 3
};

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

// ============================================================================
// 用户首页 / 打印流程 — 消息载荷契约（POD，二进制序列化）
// ============================================================================

enum class UserHomeStatus : uint32_t {
    Idle     = 0,
    Ready    = 1,
    Printing = 2,
    Error    = 3,
};

/** APP_Update_Ntf 载荷：首页环境/状态快照（Inline） */
struct EnvSnapshotPayload {
    char     device_name[32];
    uint32_t status;            /**< UserHomeStatus */
    uint8_t  wifi_connected;
    int32_t  signal_bars;
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
};

/** APP_PM25_Rsp 载荷：PM2.5 检测结果（Inline） */
struct Pm25ResultPayload {
    uint8_t is_high;
    float   pm25_cabin;
};

/** APP_CLOSE_DOOR_Rsp / APP_OpenDoor_Rsp 载荷：动作结果（Inline） */
struct DoorResultPayload {
    uint8_t ok;
};

/** 单个可打印文件项（与 ui::control_file_t 字段对齐；由 Fs_DirOpen_Rsp 一次性返回） */
struct FileEntryPayload {
    uint32_t index;          /**< 目录列表中的序号（0-based，与 UI_Print_Start_Req 一致） */
    char     name[64];
    char     path[128];
    uint32_t duration_min;
    float    material_g;
};

/** Fs_DirOpen_Rsp BufferRef 布局：[FsDirHeader][FileEntryPayload × count] */
struct FsDirHeader {
    uint32_t count;
};

/** UI_Print_Start_Req 载荷：按索引启动打印（Inline） */
struct PrintStartReqPayload {
    uint32_t file_index;
};

/** APP_Print_Progress_Ntf 载荷：打印进度推送（Inline） */
struct PrintProgressPayload {
    int32_t  progress_percent;
    uint32_t remaining_minutes;
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
};

} // namespace middleware

#endif

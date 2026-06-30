#ifndef MSG_TYPES_HPP
#define MSG_TYPES_HPP

/**
 * @file msg_types.hpp
 * @brief 跨进程消息契约（ModuleId / MsgType / Payload / Message）
 *
 * 命名规范与消息总表见：common/contract/README.md
 * 业务代码通过 msg_port.hpp 收发，勿直接操作 IPC 传输层。
 */

#include "buffer_pool.hpp"
#include <cstdint>
#include <cstring>
#include <array>

namespace middleware {

/** 进程身份；新增后端时追加枚举并更新 contract/README.md §5 */
enum class ModuleId : uint32_t {
    UI         = 0x01,
    APP_GLAVO  = 0x02,  /**< 振镜/串口后端 rongsu_glavo */
    APP_CAMERA = 0x03,  /**< 相机 / test_camera 伪相机后端 */
};

/**
 * 消息类型。命名：\<域\>_\<动作\>_\<Req|Rsp|Ntf|Done\>
 * - UI_*   ：UI 发起
 * - APP_*  ：Glavo 发起（历史名，等价于规范中的 GLAVO_*，新消息优先 GLAVO_*）
 * - Fs_*   ：文件系统（历史名，规范为 FS_*）
 */
enum class MsgType : uint32_t {
    // ---- GLAVO → UI：环境与状态推送 ----
    APP_Update_Ntf = 0x1000,  /**< [GLAVO_Env_Update_Ntf] EnvSnapshotPayload */

    // ---- UI → GLAVO：舱体检测 ----
    UI_PM25_Req,
    APP_PM25_Rsp,             /**< [GLAVO_PM25_Check_Rsp] Pm25ResultPayload */
    UI_CLOSE_DOOR_Req,
    APP_CLOSE_DOOR_Rsp,       /**< [GLAVO_Door_Close_Rsp] DoorResultPayload */

    // ---- UI ↔ GLAVO：文件系统 ----
    Fs_DirOpen_Req,           /**< [FS_Dir_Open_Req] FsDirOpenReqPayload */
    Fs_DirOpen_Rsp,           /**< [FS_Dir_Open_Rsp] FsDirHeader + FileEntryPayload[] */
    Fs_Delete_Ntf,            /**< [FS_Delete_Ntf] FsDeleteNtfPayload：删除 Local 文件 */

    // ---- UI ↔ GLAVO：舱体控制 ----
    UI_OpenDoor_Req,
    APP_OpenDoor_Rsp,         /**< [GLAVO_Door_Open_Rsp] DoorResultPayload */
    UI_Purify_Req,
    APP_Purify_Progress,      /**< [GLAVO_Purify_Progress_Ntf] Value */
    APP_Purify_Rsp,           /**< [GLAVO_Purify_Done] Empty */

    // ---- UI ↔ GLAVO：打印流程 ----
    UI_Print_Prepare_Req,     /**< PrintPrepareReqPayload：轻量预处理 + USB 复制 */
    APP_Print_Prepare_Rsp,    /**< PrintPrepareRspPayload */
    UI_Parser_Exit_Ntf,       /**< Empty：取消解析（D1 X / 二次确认 Cancel） */
    UI_Print_Start_Req,       /**< PrintStartReqPayload：正式解析 + 下发 */
    APP_Print_Progress_Ntf,   /**< [GLAVO_Print_Progress_Ntf] PrintProgressPayload */
    UI_Print_Terminate_Req,
    APP_Print_Terminate_Done, /**< [GLAVO_Print_Terminate_Done] Empty */

    // ---- UI ↔ GLAVO：耗材/滤芯 ----
    UI_Supply_Status_Req,     /**< Empty */
    APP_Supply_Status_Rsp,    /**< [GLAVO_Supply_Status_Rsp] SupplyStatusPayload */

    // ---- UI ↔ CAMERA：视频流（控制面 socket；像素走 SHM） ----
    UI_Camera_Stream_Start_Req, /**< Empty 或 CameraStreamStartReqPayload */
    APP_Camera_Stream_Start_Rsp,         /**< CameraStreamInfoPayload */
    UI_Camera_Stream_Stop_Req,           /**< Empty */
    APP_Camera_Stream_Stop_Done,         /**< Empty */
    APP_Camera_Frame_Ntf,                /**< CameraFrameNtfPayload（可选唤醒 UI） */

    // ---- GLAVO → CAMERA：打印层拍照录像（控制面；像素仍走预览 SHM） ----
    APP_Print_Video_Session_Start_Ntf, /**< PrintVideoSessionPayload */
    APP_Print_Layer_Snap_Ntf,                   /**< PrintVideoLayerSnapPayload */
    APP_Print_Video_Session_Finish_Ntf,         /**< PrintVideoSessionPayload */
    APP_Print_Video_Session_Abort_Ntf,          /**< PrintVideoSessionPayload */

    // ---- UI ↔ GLAVO：MCU OTA（测试） ----
    UI_Mcu_Ota_Start_Req,       /**< Empty：扫描 /home/image 最新 .bin 并下发 */
    UI_Mcu_Ota_Cancel_Ntf,      /**< Empty：测试用，SOC 侧中止 OTA 会话 */
    APP_Mcu_Ota_Status_Ntf,     /**< McuOtaStatusPayload */
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
// Payload 契约（POD，可 memcpy；跨进程由 ipc_wire 序列化）
// 命名：\<动作\>Payload / \<动作\>ReqPayload / \<动作\>RspPayload
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

/** 打印文件来源（DirOpen / Prepare / Start 共用；Local 列表来自 config.json） */
enum class PrintFileSource : uint8_t {
    Local = 0,
    Usb   = 1,
};

/** Fs_DirOpen_Req：UI 只传来源序号，目录路径由 Glavo 配置 */
struct FsDirOpenReqPayload {
    PrintFileSource list_source;
    uint8_t         reserved[3];
};

/** 单个可打印文件项（与 ui::control_file_t 字段对齐；由 Fs_DirOpen_Rsp 一次性返回） */
struct FileEntryPayload {
    uint32_t index;          /**< 目录列表中的序号（0-based） */
    char     name[64];
    char     path[128];
    uint32_t duration_min;   /**< 预估耗时（分钟）；本期可为 0 */
    float    material_g;     /**< 预估耗材（克）；本期可为 0 */
    uint32_t file_size;      /**< 文件大小（字节） */
    uint32_t line_count;     /**< 轻量预处理行数 */
};

/** Fs_DirOpen_Rsp BufferRef 布局：[FsDirHeader][FileEntryPayload × count] */
struct FsDirHeader {
    uint32_t count;
};

/** APP_Supply_Status_Rsp 载荷：耗材/滤芯可用度（Inline） */
struct SupplyStatusPayload {
    float   material_pct;   /**< 耗材余量 0..100 */
    float   filter_pct;     /**< 滤芯余量 0..100 */
    uint8_t material_ok;    /**< 1=充足 0=不足 */
    uint8_t filter_ok;      /**< 1=充足 0=不足 */
};

/** UI_Print_Prepare_Req / UI_Print_Start_Req 共用：按 Tab 来源 + 索引 */
struct PrintPrepareReqPayload {
    uint32_t        file_index;
    PrintFileSource file_source;
    uint8_t         reserved[3];
};

struct PrintPrepareRspPayload {
    int8_t ok;   /**< 1=成功，0=失败 */
    uint8_t reserved[3];
};

/** UI_Print_Start_Req 载荷：与 Prepare 相同（来源 + 索引） */
using PrintStartReqPayload = PrintPrepareReqPayload;

/** Fs_Delete_Ntf 载荷：文件来源 + 索引（Local 列表删除） */
using FsDeleteNtfPayload = PrintPrepareReqPayload;

/** APP_Print_Progress_Ntf 载荷：打印进度推送（Inline） */
struct PrintProgressPayload {
    int32_t  progress_percent;
    uint32_t remaining_minutes;
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
};

/** UI_Camera_Stream_Start_Req：可选 basename；空串则用后端默认 demo_1024x600.nv12 */
struct CameraStreamStartReqPayload {
    char video_name[64];
};

/** APP_Camera_Stream_Start_Rsp：SHM 帧环参数（Inline） */
struct CameraStreamInfoPayload {
    char     shm_name[64];        /**< SHM 状态环名称 */
    char     dma_socket_path[64]; /**< ABI 保留；fd 经 IPC SCM_RIGHTS 传递 */
    uint32_t width;
    uint32_t height;
    uint32_t stride_y;            /**< Y 平面实际 stride（字节） */
    uint32_t format;              /**< NV12 fourcc，见 upc_ipc::kDmaFrameFormatNv12 */
    uint32_t fps;
    uint32_t dma_buf_count;       /**< V4L2 buffer 数量（与 IPC 附带 fd 数一致） */
    int8_t   ok;                  /**< 1=成功，0=失败 */
    uint8_t  dma_mode;            /**< 固定为 1：dma fd 旁路模式；SHM 仅保留元数据 */
    uint8_t  reserved[2];
};

/** APP_Camera_Frame_Ntf：新帧序号（Inline；像素在 SHM） */
struct CameraFrameNtfPayload {
    uint32_t seq;
};

/** GLAVO→CAMERA 打印录像会话（Start/Finish/Abort 共用） */
struct PrintVideoSessionPayload {
    char     session_id[32];
    char     job_name[64];   /**< 用于输出 MP4 文件名 */
    uint32_t width;          /**< 0=Camera 默认 */
    uint32_t height;         /**< 0=Camera 默认 */
    uint32_t layer_fps;      /**< MP4 层帧率；0=默认 */
};

/** GLAVO→CAMERA 单层拍照（换层排空后、LAYER_DONE 前） */
struct PrintVideoLayerSnapPayload {
    char     session_id[32];
    uint32_t layer_index;    /**< 1-based */
    uint16_t layer_height_um;
    uint16_t reserved;
};

/** APP_Mcu_Ota_Status_Ntf：MCU OTA 进度/状态（测试 UI） */
enum class McuOtaState : uint8_t {
    Idle         = 0,
    Preparing    = 1,
    Transferring = 2,
    Verifying    = 3,
    Success      = 4,
    Failed       = 5,
    Cancelled    = 6,
};

struct McuOtaStatusPayload {
    McuOtaState state;
    uint8_t     progress;   /**< MCU byte[9] 0~255 */
    uint8_t     reserved[2];
};

} // namespace middleware

#endif

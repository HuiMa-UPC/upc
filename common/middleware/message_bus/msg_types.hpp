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
 *
 * 【重要】这是 UI / Glavo / Camera 三进程共享的线协议枚举。每个分段的首个成员都
 * 显式赋基址（0x10x0 递增），段内成员自动递增。新增消息只能【追加到所在分段末尾】，
 * 严禁在分段中间插入或删除——否则会改变其后成员的数值，造成跨进程枚举错位、消息
 * 无法路由。若某段将写满，请新开一个基址分段，不要挤占相邻段。
 */
enum class MsgType : uint32_t {
    // ---- GLAVO → UI：环境与状态推送 ----
    APP_Update_Ntf = 0x1000,  /**< [GLAVO_Env_Update_Ntf] EnvSnapshotPayload */

    // ---- UI → GLAVO：舱体检测 ----
    UI_PM25_Req = 0x1010,
    APP_PM25_Rsp,             /**< [GLAVO_PM25_Check_Rsp] Pm25ResultPayload */
    UI_CLOSE_DOOR_Req,
    APP_CLOSE_DOOR_Rsp,       /**< [GLAVO_Door_Close_Rsp] DoorResultPayload */

    // ---- UI ↔ GLAVO：文件系统 ----
    Fs_DirOpen_Req = 0x1020,  /**< [FS_Dir_Open_Req] FsDirOpenReqPayload */
    Fs_DirOpen_Rsp,           /**< [FS_Dir_Open_Rsp] FsDirHeader + FileEntryPayload[] */
    Fs_Delete_Ntf,            /**< [FS_Delete_Ntf] FsDeleteNtfPayload：删除 Local 文件 */

    // ---- UI ↔ GLAVO：舱体控制 ----
    UI_OpenDoor_Req = 0x1030,
    APP_OpenDoor_Rsp,         /**< [GLAVO_Door_Open_Rsp] DoorResultPayload */
    UI_Purify_Req,
    APP_Purify_Progress,      /**< [GLAVO_Purify_Progress_Ntf] Value */
    APP_Purify_Rsp,           /**< [GLAVO_Purify_Done] Empty */

    // ---- UI ↔ GLAVO：打印流程 ----
    UI_Print_Prepare_Req = 0x1040, /**< PrintPrepareReqPayload：轻量预处理 + USB 复制 */
    APP_Print_Prepare_Rsp,    /**< PrintPrepareRspPayload */
    UI_Parser_Exit_Ntf,       /**< Empty：取消解析（D1 X / 二次确认 Cancel） */
    UI_Print_Start_Req,       /**< PrintStartReqPayload：正式解析 + 下发 */
    APP_Print_Progress_Ntf,   /**< [GLAVO_Print_Progress_Ntf] PrintProgressPayload */
    UI_Print_Terminate_Req,
    APP_Print_Terminate_Done, /**< [GLAVO_Print_Terminate_Done] Empty */
    UI_Print_Pause_Req,       /**< Empty：暂停打印（McuLink 停止 TCP 运动下发） */
    UI_Print_Resume_Req,      /**< Empty：恢复打印 */
    APP_Print_Pause_Rsp,      /**< [GLAVO_Print_Pause_Rsp] PrintPauseStatePayload：暂停/恢复响应（Pause/Resume Req 共用） */

    // ---- UI ↔ GLAVO：耗材/滤芯 ----
    UI_Supply_Status_Req = 0x1060, /**< Empty */
    APP_Supply_Status_Rsp,    /**< [GLAVO_Supply_Status_Rsp] SupplyStatusPayload */

    // ---- UI ↔ CAMERA：视频流（控制面 socket；像素走 SHM） ----
    UI_Camera_Stream_Start_Req = 0x1070, /**< Empty 或 CameraStreamStartReqPayload */
    APP_Camera_Stream_Start_Rsp,         /**< CameraStreamInfoPayload */
    UI_Camera_Stream_Stop_Req,           /**< Empty */
    APP_Camera_Stream_Stop_Done,         /**< Empty */
    APP_Camera_Frame_Ntf,                /**< CameraFrameNtfPayload（可选唤醒 UI） */

    // ---- GLAVO → CAMERA：打印层拍照录像（控制面；像素仍走预览 SHM） ----
    APP_Print_Video_Session_Start_Ntf = 0x1080, /**< PrintVideoSessionPayload */
    APP_Print_Layer_Snap_Ntf,                   /**< PrintVideoLayerSnapPayload */
    APP_Print_Video_Session_Finish_Ntf,         /**< PrintVideoSessionPayload */
    APP_Print_Video_Session_Abort_Ntf,          /**< PrintVideoSessionPayload */

    // ---- UI ↔ GLAVO：MCU OTA（测试） ----
    UI_Mcu_Ota_Start_Req = 0x1090, /**< Empty：扫描 /home/image 最新 .bin 并下发 */
    UI_Mcu_Ota_Cancel_Ntf,      /**< Empty：测试用，SOC 侧中止 OTA 会话 */
    APP_Mcu_Ota_Status_Ntf,     /**< McuOtaStatusPayload */

    // ---- UI → GLAVO：调试页手动控制（后端待实现，前端先发送） ----
    UI_Debug_Axis_Jog_Req = 0x1100, /**< DebugAxisJogPayload：点动/长按点动 */
    UI_Debug_Axis_Hold_Stop_Req,    /**< DebugAxisPayload：长按松手停 */
    UI_Debug_Axis_SetZero_Req,      /**< DebugAxisPayload：设工作零点（Z 轴） */
    UI_Debug_Axis_Home_Req,         /**< DebugAxisPayload：轴回 Home/归零（Z/S） */
    UI_Debug_Galvo_Move_Req,        /**< DebugGalvoMovePayload：振镜方向步进 */
    UI_Debug_Galvo_Home_Req,        /**< Empty：振镜回中/复位 */
    UI_Debug_Galvo_RedLight_Req,    /**< DebugOnOffPayload：红光开关 */
    UI_Debug_Galvo_Param_Save_Req,  /**< DebugGalvoParamPayload：保存 Scale/步长 */
    UI_Debug_Galvo_Calib_Req,       /**< Empty：开始标定测试 */
    UI_Debug_Device_Req,            /**< DebugDevicePayload：设备/门/电机/照明 */
    UI_Debug_Powder_Bucket_Req,     /**< DebugBucketPayload：粉桶开关 */
    UI_Debug_Pusher_Req,            /**< DebugMotorPayload：筛粉电机 停止/运行/反转倒渣 */
    UI_Debug_Recycle_Motor_Req,     /**< DebugMotorPayload：内门 停止/开/关（原回粉电机块改为内门） */
    UI_Debug_Fan_Req,               /**< DebugFanPayload：风扇 */
    UI_Debug_Switch_Req,            /**< DebugSwitchPayload：激光/洗气/底板加热 */
    UI_Debug_CleanTest_Start_Req,   /**< Empty：清粉测试开启（关闭暂不使用） */
    UI_Debug_Fan_Duty_Req,          /**< DebugFanPayload：仅设风扇占空比（不开关） */
    UI_Debug_PrintParam_Set_Req,    /**< DebugPrintParamPayload：打印层参数 */
    UI_Debug_Laser_Power_Set_Req,   /**< DebugLaserPowerPayload：激光手动功率 */
    UI_Debug_Laser_Power_Reset_Req, /**< Empty：取消手动功率，恢复外控 */
    UI_Debug_Laser_AlarmReset_Req,  /**< Empty：激光器报警复位（固定 1301） */

    // ---- GLAVO → UI：调试页状态回读（按模块拆分） ----
    GLAVO_Debug_Sensor_Ntf = 0x1200, /**< DebugSensorPayload：顶部 7 路传感器 */
    GLAVO_Debug_Axis_Ntf,           /**< DebugAxisStatePayload：Z/S/D 位置 + Z 工作坐标 */
    GLAVO_Debug_Device_Ntf,         /**< DebugDeviceStatePayload：设备/门/电机/照明 */
    GLAVO_Debug_Galvo_Ntf,          /**< DebugGalvoStatePayload：振镜 X/Y + 红光 */
    GLAVO_Debug_PowderBucket_Ntf,   /**< DebugPowderBucketStatePayload */
    GLAVO_Debug_Pusher_Ntf,         /**< DebugPusherStatePayload */
    GLAVO_Debug_RecycleMotor_Ntf,   /**< DebugRecycleMotorStatePayload */
    GLAVO_Debug_Fan_Ntf,            /**< DebugFanStatePayload */
    GLAVO_Debug_Switch_Ntf,         /**< DebugSwitchStatePayload */
    GLAVO_Debug_Laser_Ntf,          /**< DebugLaserStatePayload：激光器状态/报警/功率/温度 */
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

/** APP_Print_Pause_Rsp 载荷：暂停/恢复状态响应（Inline；Pause/Resume Req 共用） */
struct PrintPauseStatePayload {
    uint8_t paused;   /**< 1=已暂停 0=已恢复 */
    uint8_t reserved[3];
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

// ============================================================================
// 调试页（手动控制/标定）契约 —— 前端发送，后端（Glavo）待实现
// 命名见 common/README.md §3；枚举跨进程时按 uint8_t 传输
// ============================================================================

enum class DebugAxis        : uint8_t { Z = 0, S = 1, D = 2 };
enum class DebugJogMode     : uint8_t { Click = 0, Hold = 1 };
enum class DebugGalvoAxis   : uint8_t { X = 0, Y = 1 };
enum class DebugDeviceTarget: uint8_t { Device = 0, Door = 1, Motor = 2, Light = 3 };
enum class DebugMotorAction : uint8_t { Stop = 0, Forward = 1, Reverse = 2 };
enum class DebugSwitchTarget: uint8_t { Laser = 0, GasWash = 1, BaseHeat = 2 };
enum class DebugPowderStatus: uint8_t { Available = 0, Busy = 1, Fault = 2 };

/** UI_Debug_Axis_Jog_Req：轴点动 / 长按点动 */
struct DebugAxisJogPayload {
    uint8_t axis;       /**< DebugAxis */
    uint8_t mode;       /**< DebugJogMode：Click=单步，Hold=持续 */
    int8_t  dir;        /**< +1 / -1 */
    uint8_t reserved;
    float   value;      /**< Click=步距(mm/°)，Hold=速度(mm·°/s) */
};

/** UI_Debug_Axis_Hold_Stop_Req / UI_Debug_Axis_SetZero_Req */
struct DebugAxisPayload {
    uint8_t axis;       /**< DebugAxis */
    uint8_t reserved[3];
};

/** UI_Debug_Galvo_Move_Req：振镜方向步进 */
struct DebugGalvoMovePayload {
    uint8_t axis;       /**< DebugGalvoAxis */
    int8_t  dir;        /**< +1 / -1 */
    uint8_t reserved[2];
    float   step_mm;
};

/** 通用开关：UI_Debug_Galvo_RedLight_Req / UI_Debug_Pusher_Req */
struct DebugOnOffPayload {
    uint8_t on;
    uint8_t reserved[3];
};

/** UI_Debug_Galvo_Param_Save_Req：保存 Scale / 步长 */
struct DebugGalvoParamPayload {
    float scale;
    float step_mm;
};

/** UI_Debug_Device_Req：设备/门/电机/照明 */
struct DebugDevicePayload {
    uint8_t target;     /**< DebugDeviceTarget */
    uint8_t on;
    uint8_t reserved[2];
};

/** UI_Debug_Powder_Bucket_Req：粉桶开关 */
struct DebugBucketPayload {
    uint8_t bucket_id;  /**< 1 / 2 */
    uint8_t on;
    uint8_t reserved[2];
};

/** UI_Debug_Recycle_Motor_Req：回粉电机 */
struct DebugMotorPayload {
    uint8_t action;     /**< DebugMotorAction */
    uint8_t reserved[3];
};

/** UI_Debug_Fan_Req：风扇开关 + 占空比 */
struct DebugFanPayload {
    uint8_t on;
    uint8_t reserved[3];
    float   duty;       /**< 0..100 (%) */
};

/** UI_Debug_Switch_Req：激光/洗气/底板加热 */
struct DebugSwitchPayload {
    uint8_t target;     /**< DebugSwitchTarget */
    uint8_t on;
    uint8_t reserved[2];
};

/** UI_Debug_PrintParam_Set_Req：打印层参数（层厚 + 平衡系数×1000） */
struct DebugPrintParamPayload {
    uint16_t layer_um;        /**< 层厚 um */
    uint16_t balance_x1000;   /**< 平衡系数 × 1000 */
};

/** UI_Debug_Laser_Power_Set_Req：激光手动功率百分比 */
struct DebugLaserPowerPayload {
    uint8_t percent;          /**< 0..100 */
    uint8_t reserved[3];
};

/** GLAVO_Debug_Laser_Ntf：激光器状态上报（原始值，界面自行换算） */
struct DebugLaserStatePayload {
    uint16_t status;          /**< bit0 红光/bit1 激光输出/bit2 Ready/bit3 外部输出有效 */
    uint16_t alarm;           /**< laser_alarm 打包报警位 */
    uint16_t power_raw;       /**< 功率原始值 0..255 */
    uint16_t pump_temp_raw;   /**< 泵浦温度原始值 */
};

/** GLAVO_Debug_Sensor_Ntf：顶部 7 路传感器快照 */
struct DebugSensorPayload {
    float   oxygen_percent;
    float   pressure_kpa;
    float   pm25;
    float   humidity_pct;
    float   cabin_temp_c;
    float   baseplate_temp_c;
    uint8_t powder_status;   /**< DebugPowderStatus */
    uint8_t reserved[3];
};

/** GLAVO_Debug_Axis_Ntf：Z/S/D 实际位置 + Z 工作坐标 */
struct DebugAxisStatePayload {
    float z_pos_mm;
    float s_pos_mm;
    float d_pos_deg;
    float z_work_zero_mm;
};

/** GLAVO_Debug_Device_Ntf：设备快捷行状态 */
struct DebugDeviceStatePayload {
    uint8_t device_awake;
    uint8_t door_open;
    uint8_t motor_on;
    uint8_t light_on;
};

/** GLAVO_Debug_Galvo_Ntf：振镜 X/Y + 红光 */
struct DebugGalvoStatePayload {
    float   x;
    float   y;
    uint8_t red_light_on;
    uint8_t reserved[3];
};

struct DebugPowderBucketStatePayload {
    uint8_t bucket1_open;
    uint8_t bucket2_open;
    uint8_t reserved[2];
};

struct DebugPusherStatePayload {
    uint8_t pusher_on;
    uint8_t reserved[3];
};

struct DebugRecycleMotorStatePayload {
    uint8_t state;      /**< DebugMotorAction */
    uint8_t reserved[3];
};

struct DebugFanStatePayload {
    uint8_t on;
    uint8_t reserved[3];
    float   duty;
};

struct DebugSwitchStatePayload {
    uint8_t laser_on;
    uint8_t gas_wash_on;
    uint8_t base_heat_on;
    uint8_t reserved;
};

} // namespace middleware

#endif

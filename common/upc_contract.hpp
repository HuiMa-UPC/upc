#ifndef UPC_CONTRACT_HPP
#define UPC_CONTRACT_HPP

/**
 * @file upc_contract.hpp
 * @brief UPC 跨进程唯一契约头文件（IPC 配置 / 日志开关 / 进程注册 / 线协议 ABI / MsgType / Payload）
 *
 * 新增 ModuleId、MsgType、Payload、socket 路径、日志模块、进程条目时【只改本文件】，
 * 并同步 common/COMMON.md 消息总表（contract 四步流程）。
 *
 * 运行时 Message / MsgPayload（依赖 BufferPool）见 msg_runtime.hpp；
 * 业务 IPC 入口见 ipc/msg_port.hpp。
 */

#include <cstddef>
#include <cstdint>

// ============================================================================
// §1 IPC 默认配置（环境变量 UPC_IPC_SOCKET 等可覆盖运行时路径）
// ============================================================================

/** 中央 Broker listen socket（环境变量 UPC_BROKER_SOCKET 可覆盖） */
#ifndef UPC_BROKER_SOCKET_DEFAULT
#define UPC_BROKER_SOCKET_DEFAULT "/tmp/upc_broker.sock"
#endif

/** UI hub 模式下，已断开的 peer 后台重连间隔（毫秒） */
#ifndef UPC_IPC_PEER_RECONNECT_INTERVAL_MS
#define UPC_IPC_PEER_RECONNECT_INTERVAL_MS 3000
#endif

/** rongsu.sh 等待后端 listen socket 出现的超时（毫秒） */
#ifndef UPC_IPC_PEER_STARTUP_WAIT_MS
#define UPC_IPC_PEER_STARTUP_WAIT_MS 10000
#endif

/** Broker listen backlog（accept 队列深度） */
#ifndef UPC_IPC_LISTEN_BACKLOG
#define UPC_IPC_LISTEN_BACKLOG 8
#endif

// ============================================================================
// §2 调试日志子开关（总开关 UPC_DEBUG 由根 CMakeLists 注入）
// ============================================================================

/**
 * 总开关 UPC_DEBUG 由根 CMakeLists 注入（ON=1 / OFF=0）。
 * 子开关 UPC_MOD_* 仅在此设默认值；关闭某模块：-DUPC_MOD_MCU_LINK=0
 * 逻辑：UPC_DEBUG=OFF 时所有日志宏为空；ON 时再看对应 UPC_MOD_* 是否为 1。
 *
 * 注意：UPC_LOG_* 为打印宏，UPC_MOD_* 为模块子开关，勿混用同名。
 */

#ifndef UPC_DEBUG
#define UPC_DEBUG 1
#endif

#if UPC_DEBUG

#ifndef UPC_MOD_MSG_BUS
#define UPC_MOD_MSG_BUS 0
#endif
#ifndef UPC_MOD_IPC
#define UPC_MOD_IPC 0
#endif
#ifndef UPC_MOD_MEM_POOL
#define UPC_MOD_MEM_POOL 0
#endif
#ifndef UPC_MOD_BUFFER_POOL
#define UPC_MOD_BUFFER_POOL 0
#endif
#ifndef UPC_MOD_GLAVO_SERVICE
#define UPC_MOD_GLAVO_SERVICE 1
#endif
#ifndef UPC_MOD_MCU_LINK
#define UPC_MOD_MCU_LINK 1
#endif
#ifndef UPC_MOD_CAL_PARSER
#define UPC_MOD_CAL_PARSER 1
#endif
#ifndef UPC_MOD_PARSER_CONTEXT
#define UPC_MOD_PARSER_CONTEXT 1
#endif
#ifndef UPC_MOD_GCODE_WORKER
#define UPC_MOD_GCODE_WORKER 1
#endif
#ifndef UPC_MOD_OVF_WORKER
#define UPC_MOD_OVF_WORKER 0
#endif
#ifndef UPC_MOD_SERVICE_BASE
#define UPC_MOD_SERVICE_BASE 1
#endif
#ifndef UPC_MOD_SYS_MONITOR
#define UPC_MOD_SYS_MONITOR 1
#endif
#ifndef UPC_MOD_CAMERA_SERVICE
#define UPC_MOD_CAMERA_SERVICE 1
#endif
#ifndef UPC_MOD_CAMERA_PREVIEW
#define UPC_MOD_CAMERA_PREVIEW 1
#endif
#ifndef UPC_MOD_MCU_OTA
#define UPC_MOD_MCU_OTA 1
#endif
#ifndef UPC_MOD_XFR_SERVICE
#define UPC_MOD_XFR_SERVICE 1
#endif
#ifndef UPC_MOD_VISUAL_SERVICE
#define UPC_MOD_VISUAL_SERVICE 1
#endif
#ifndef UPC_MOD_MQTT_SERVICE
#define UPC_MOD_MQTT_SERVICE 1
#endif
#ifndef UPC_MOD_MEDIA_SERVICE
#define UPC_MOD_MEDIA_SERVICE 1
#endif
#ifndef UPC_MOD_ALARM_STATE
#define UPC_MOD_ALARM_STATE 1
#endif
/** Glavo: TCP 心跳/状态应答日志默认开关（环境变量可覆盖） */
#ifndef UPC_GLAVO_LOG_TCP_HEARTBEAT_RSP_DEFAULT
#define UPC_GLAVO_LOG_TCP_HEARTBEAT_RSP_DEFAULT 0
#endif
/** Glavo: 以太网 TCP 下发帧日志默认开关（环境变量可覆盖） */
#ifndef UPC_GLAVO_LOG_TCP_TX_DEFAULT
#define UPC_GLAVO_LOG_TCP_TX_DEFAULT 0
#endif
/** Glavo: 激光功率包下发日志默认开关（环境变量可覆盖） */
#ifndef UPC_GLAVO_LOG_LASER_POWER_DEFAULT
#define UPC_GLAVO_LOG_LASER_POWER_DEFAULT 1
#endif
/**
 * Media: 直播链路运行期心跳统计日志（`h264_enc/live_rtsp/local_rtsp stat`，5s 一行）。
 * 排查"画面卡住/延迟大"时靠它定位断点，平时嫌吵可置 0；
 * 关闭只停打印，计数仍在累加，停流时的汇总日志不受影响。
 */
#ifndef UPC_MEDIA_LOG_STREAM_STAT
#define UPC_MEDIA_LOG_STREAM_STAT 0
#endif

#else /* !UPC_DEBUG */

#define UPC_MOD_MSG_BUS 0
#define UPC_MOD_IPC 0
#define UPC_MOD_MEM_POOL 0
#define UPC_MOD_BUFFER_POOL 0
#define UPC_MOD_GLAVO_SERVICE 0
#define UPC_MOD_MCU_LINK 0
#define UPC_MOD_CAL_PARSER 0
#define UPC_MOD_PARSER_CONTEXT 0
#define UPC_MOD_GCODE_WORKER 0
#define UPC_MOD_OVF_WORKER 0
#define UPC_MOD_SERVICE_BASE 0
#define UPC_MOD_SYS_MONITOR 0
#define UPC_MOD_CAMERA_SERVICE 0
#define UPC_MOD_CAMERA_PREVIEW 0
#define UPC_MOD_MCU_OTA 0
#define UPC_MOD_XFR_SERVICE 0
#define UPC_MOD_VISUAL_SERVICE 0
#define UPC_MOD_MQTT_SERVICE 0
#define UPC_MOD_MEDIA_SERVICE 0
#define UPC_MOD_ALARM_STATE 0
#define UPC_GLAVO_LOG_TCP_HEARTBEAT_RSP_DEFAULT 0
#define UPC_GLAVO_LOG_TCP_TX_DEFAULT 0
#define UPC_GLAVO_LOG_LASER_POWER_DEFAULT 0
#define UPC_MEDIA_LOG_STREAM_STAT 0

#endif /* UPC_DEBUG */

// ============================================================================
// §2.5 进程日志开关（默认值；可被环境变量 UPC_LOG_PROC_* 覆盖）
// ============================================================================
/**
 * 说明：
 * - UPC_LOG_PROC_DEFAULT：未单独指定进程时的默认开关
 * - UPC_LOG_PROC_<PROC>：按进程名细分，1=开，0=关
 * - 进程名与 ProcessConfig::name 对齐（如 rongsu_glavo -> RONGSU_GLAVO）
 */
#ifndef UPC_LOG_PROC_DEFAULT
#define UPC_LOG_PROC_DEFAULT 1
#endif

#ifndef UPC_LOG_PROC_RONGSU_UI
#define UPC_LOG_PROC_RONGSU_UI UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_GLAVO
#define UPC_LOG_PROC_RONGSU_GLAVO UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_CAMERA
#define UPC_LOG_PROC_RONGSU_CAMERA UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_BROKER
#define UPC_LOG_PROC_RONGSU_BROKER UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_XFR
#define UPC_LOG_PROC_RONGSU_XFR UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_VISUAL
#define UPC_LOG_PROC_RONGSU_VISUAL UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_MQTT
#define UPC_LOG_PROC_RONGSU_MQTT UPC_LOG_PROC_DEFAULT
#endif
#ifndef UPC_LOG_PROC_RONGSU_MEDIA
#define UPC_LOG_PROC_RONGSU_MEDIA UPC_LOG_PROC_DEFAULT
#endif

// ============================================================================
// §3 进程注册表（PID / 日志路径；daemon 实现见 process/process_ctl.hpp）
// ============================================================================

struct ProcessConfig {
    const char* pid_file;
    const char* name;
};

/** 每进程独立 PID；守护进程日志统一写入 /var/log/topic 专题文件。 */
inline ProcessConfig process_config_ui() {
    return ProcessConfig{
        "/var/run/rongsu_ui.pid",
        "rongsu_ui",
    };
}

inline ProcessConfig process_config_glavo() {
    return ProcessConfig{
        "/var/run/rongsu_glavo.pid",
        "rongsu_glavo",
    };
}

inline ProcessConfig process_config_camera() {
    return ProcessConfig{
        "/var/run/rongsu_camera.pid",
        "rongsu_camera",
    };
}

inline ProcessConfig process_config_broker() {
    return ProcessConfig{
        "/var/run/rongsu_broker.pid",
        "rongsu_broker",
    };
}

inline ProcessConfig process_config_xfr() {
    return ProcessConfig{
        "/var/run/rongsu_xfr.pid",
        "rongsu_xfr",
    };
}

inline ProcessConfig process_config_visual() {
    return ProcessConfig{
        "/var/run/rongsu_visual.pid",
        "rongsu_visual",
    };
}

inline ProcessConfig process_config_mqtt() {
    return ProcessConfig{
        "/var/run/rongsu_mqtt.pid",
        "rongsu_mqtt",
    };
}

inline ProcessConfig process_config_media() {
    return ProcessConfig{
        "/var/run/rongsu_media.pid",
        "rongsu_media",
    };
}

// ============================================================================
// §4 线协议 ABI 常量（帧格式实现见 ipc/ipc_wire.hpp）
// ============================================================================

namespace upc_ipc {

constexpr uint32_t WIRE_MAGIC   = 0x31504355u;
constexpr uint32_t WIRE_VERSION = 1u;
constexpr std::size_t WIRE_MAX_PAYLOAD = 65536u;

/** wire 帧 + SCM_RIGHTS 复合包魔数（小端 "UPDF"），与 [len][body] 普通帧区分 */
constexpr uint32_t IPC_FD_FRAME_MAGIC    = 0x46445055u;
constexpr uint32_t IPC_FD_FRAME_MAX_FDS  = 32u;

/** connect 后首帧 reg（不进 MsgType）；小端 "UPRG" */
constexpr uint32_t IPC_REG_MAGIC         = 0x47525055u;
constexpr uint32_t IPC_REG_VERSION       = 1u;
/** Broker → client 上下线通知；小端 "URNF" */
constexpr uint32_t IPC_REG_NOTIFY_MAGIC  = 0x464E5255u;
constexpr uint32_t IPC_REG_EVENT_ONLINE  = 1u;
constexpr uint32_t IPC_REG_EVENT_OFFLINE = 2u;

enum class WirePayloadType : uint8_t {
    Empty  = 0,
    Inline = 1,
    Value  = 3,
};

} // namespace upc_ipc

// ============================================================================
// §5 消息契约（ModuleId / MsgType / POD Payload）
// ============================================================================

namespace middleware {

/** 进程身份；新增后端时追加枚举并更新 common/COMMON.md §多后端拓扑 */
enum class ModuleId : uint32_t {
    UI         = 0x01,
    APP_GLAVO  = 0x02,  /**< 振镜/串口后端 rongsu_glavo */
    APP_CAMERA = 0x03,  /**< 相机 / test_camera 伪相机后端 */
    APP_XFR    = 0x04,  /**< WiFi 传输后端 rongsu_xfr */
    APP_VISUAL = 0x05,  /**< 视觉识别后端 rongsu_visual */
    APP_MQTT   = 0x06,  /**< MQTT 网关后端 rongsu_mqtt */
    APP_MEDIA  = 0x07,  /**< 媒体后端 rongsu_media（拍照/录像/延时封装） */
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
    APP_Purify_Ntf,           /**< [GLAVO_Purify_Ntf] Value */
    APP_Purify_Rsp,           /**< [GLAVO_Purify_Rsp] Empty */
    UI_Clean_Start_Req,       /**< Empty：开启清粉（开门前 PM2.5≥50 时触发） */
    APP_Clean_Start_Rsp,      /**< Empty：清粉完成（后端风机循环至 PM2.5 合格后回） */

    // ---- UI ↔ GLAVO：打印流程 ----
    UI_Print_Prepare_Req = 0x1040, /**< PrintPrepareReqPayload：轻量预处理 + USB 复制 */
    APP_Print_Prepare_Rsp,    /**< PrintPrepareRspPayload */
    UI_Parser_Exit_Ntf,       /**< Empty：取消解析（D1 X / 二次确认 Cancel） */
    UI_Print_Start_Req,       /**< PrintStartReqPayload：正式解析 + 下发 */
    APP_Print_Progress_Ntf,   /**< [GLAVO_Print_Progress_Ntf] PrintProgressPayload */
    UI_Print_Terminate_Req,
    APP_Print_Terminate_Rsp,  /**< [GLAVO_Print_Terminate_Rsp] Empty */
    UI_Print_Pause_Req,       /**< Empty：暂停打印（McuLink 停止 TCP 运动下发） */
    UI_Print_Resume_Req,      /**< Empty：恢复打印 */
    UI_Print_Powder_Low_Ignore_Req, /**< Empty：本次打印忽略缺粉自动暂停与缺粉通知 */
    APP_Print_Pause_Rsp,      /**< [GLAVO_Print_Pause_Rsp] PrintPauseStatePayload：暂停/恢复响应（Pause/Resume Req 共用） */
    APP_Print_Powder_Low_Ntf, /**< Empty：粉量传感器 bit0/bit1 同时为 0，触发一次缺粉通知 */
    GLAVO_Print_State_Ntf = 0x104C, /**< [GLAVO→MQTT] PrintStateNtfPayload：开始打印/打印结束状态边沿（触发一次 state 上报） */

    // ---- UI ↔ GLAVO：耗材/滤芯 ----
    UI_Supply_Status_Req = 0x1060, /**< Empty */
    APP_Supply_Status_Rsp,    /**< [GLAVO_Supply_Status_Rsp] SupplyStatusPayload */

    // ---- UI ↔ CAMERA：视频流（控制面 socket；像素走 SHM） ----
    UI_Camera_Stream_Start_Req = 0x1070, /**< Empty 或 CameraStreamStartReqPayload */
    APP_Camera_Stream_Start_Rsp,         /**< CameraStreamInfoPayload */
    UI_Camera_Stream_Stop_Req,           /**< Empty */
    APP_Camera_Stream_Stop_Rsp,          /**< Empty */
    APP_Camera_Frame_Ntf,                /**< CameraFrameNtfPayload（可选唤醒 UI） */
    UI_Camera_Snapshot_Req,              /**< Empty：请求保存当前最新 NV12 帧到 /data/media/photo */
    APP_Camera_Snapshot_Rsp,             /**< CameraSnapshotRspPayload */

    // ---- GLAVO → MEDIA：打印层拍照录像（控制面；MEDIA 消费 CAMERA 预览 SHM） ----
    APP_Print_Video_Session_Start_Ntf = 0x1080, /**< PrintVideoSessionPayload */
    APP_Print_Layer_Snap_Ntf,                   /**< PrintVideoLayerSnapPayload */
    APP_Print_Video_Session_Finish_Ntf,         /**< PrintVideoSessionPayload */
    APP_Print_Video_Session_Abort_Ntf,          /**< PrintVideoSessionPayload */
    APP_Print_H264_Record_Pause_Ntf,            /**< PrintPauseStatePayload：通知 Media 暂停/恢复连续 H264 录制 */
    /* 本条方向为 MEDIA → UI（与上面四条相反）：video/config.json 已原子落盘 */
    APP_Print_Video_Manifest_Ntf,               /**< PrintVideoManifestPayload */

    // ---- UI ↔ GLAVO：MCU OTA（测试） ----
    UI_Mcu_Ota_Start_Req = 0x1090, /**< Empty：启动 Galvo(以太网) OTA，扫描 /data/image/galvo */
    UI_Mcu_Ota_Cancel_Ntf,         /**< Empty：中止 Glavo OTA 会话 */
    APP_Mcu_Ota_Status_Ntf,        /**< McuOtaStatusPayload（Glavo OTA 状态） */
    UI_Serial_Mcu_Ota_Start_Req,   /**< Empty：启动 MCU(串口) OTA，扫描 /data/image/mcu */
    UI_Serial_Mcu_Ota_Cancel_Ntf,  /**< Empty：中止 MCU 串口 OTA 会话 */
    APP_Serial_Mcu_Ota_Status_Ntf, /**< McuOtaStatusPayload（MCU 串口 OTA 状态） */

    // ---- UI → GLAVO：调试页手动控制（后端待实现，前端先发送） ----
    UI_Debug_Axis_Jog_Ntf = 0x1100, /**< DebugAxisJogPayload：点动/长按点动 */
    UI_Debug_Axis_Hold_Stop_Ntf,    /**< DebugAxisPayload：长按松手停 */
    UI_Debug_Axis_SetZero_Ntf,      /**< DebugAxisPayload：设工作零点（Z 轴） */
    UI_Debug_Axis_Home_Ntf,         /**< DebugAxisPayload：轴回 Home/归零（Z/S） */
    UI_Debug_Galvo_Move_Ntf,        /**< DebugGalvoMovePayload：振镜方向步进 */
    UI_Debug_Galvo_Home_Ntf,        /**< Empty：振镜回中/复位 */
    UI_Debug_Galvo_RedLight_Ntf,    /**< DebugOnOffPayload：红光开关 */
    UI_Debug_Galvo_Param_Save_Ntf,  /**< DebugGalvoParamPayload：保存 Scale + 中心偏移到 cal.csv */
    UI_Debug_Galvo_Calib_Ntf,       /**< Empty：开始标定测试 */
    UI_Debug_Device_Ntf,            /**< DebugDevicePayload：设备/门/电机/照明 */
    UI_Debug_Powder_Bucket_Ntf,     /**< DebugBucketPayload：粉桶开关 */
    UI_Debug_Pusher_Ntf,            /**< DebugMotorPayload：筛粉电机 停止/运行/反转倒渣 */
    UI_Debug_Recycle_Motor_Ntf,     /**< DebugMotorPayload：内门 停止/开/关（原回粉电机块改为内门） */
    UI_Debug_Fan_Ntf,               /**< DebugFanPayload：风扇 */
    UI_Debug_Switch_Ntf,            /**< DebugSwitchPayload：激光/洗气/底板加热 */
    UI_Debug_CleanTest_Start_Ntf,   /**< Empty：清粉测试开启） */
    UI_Debug_CleanTest_Close_Ntf,   /**< Empty：清粉测试关闭/中止 */
    UI_Debug_Fan_Duty_Ntf,          /**< DebugFanPayload：仅设风扇占空比（不开关） */
    UI_Debug_PrintParam_Set_Ntf,    /**< DebugPrintParamPayload：打印层参数 */
    UI_Debug_Laser_Power_Set_Ntf,   /**< DebugLaserPowerPayload：激光手动功率 */
    UI_Debug_Laser_Power_Reset_Ntf, /**< Empty：取消手动功率，恢复外控 */
    UI_Debug_Laser_AlarmReset_Ntf,  /**< Empty：激光器报警复位（固定 1301） */
    UI_Debug_Print_H264_Record_Set_Ntf, /**< DebugOnOffPayload：打印期连续 H264 录像开关（仅本次运行） */
    UI_Debug_Print_H264_Record_Get_Ntf, /**< Empty：查询当前打印期连续 H264 录像开关状态 */
    UI_Debug_Clean_Record_Set_Ntf,      /**< DebugOnOffPayload：清粉录像开关（仅本次运行） */
    UI_Debug_Clean_Record_Get_Ntf,      /**< Empty：查询当前清粉录像开关状态 */
    UI_Debug_Env_Sim_Set_Ntf,           /**< DebugOnOffPayload：环境上报模拟开关（仅本次运行，默认关） */
    UI_Debug_Env_Sim_Get_Ntf,           /**< Empty：查询当前环境上报模拟开关状态 */
    UI_Debug_Env_Sim_Powder_Set_Ntf,    /**< DebugOnOffPayload：环境模拟粉量缺粉开关（延迟 5s 生效） */
    UI_Debug_Env_Sim_Powder_Get_Ntf,    /**< Empty：查询当前环境模拟粉量缺粉开关状态 */
    UI_Debug_FirstLayer_Visual_Check_Set_Ntf, /**< DebugOnOffPayload：首层视觉检测开关（仅本次运行，默认关） */
    UI_Debug_FirstLayer_Visual_Check_Get_Ntf, /**< Empty：查询当前首层视觉检测开关状态 */
    UI_Debug_Local_Rtsp_Set_Ntf,        /**< DebugOnOffPayload：本地局域网 RTSP Server 开关（→Media，仅本次运行，默认关） */
    UI_Debug_Local_Rtsp_Get_Ntf,        /**< Empty：查询当前本地 RTSP Server 开关状态（→Media） */
    UI_Debug_TestImage_Set_Ntf,         /**< DebugOnOffPayload：红光测试图循环打印开关（/data/test/test.gcode） */
    UI_Debug_TestImage_Get_Ntf,         /**< Empty：查询当前测试图循环打印状态 */

    // ---- GLAVO → UI：调试页状态回读（按模块拆分） ----
    GLAVO_Debug_Sensor_Ntf = 0x1200, /**< DebugSensorPayload：顶部 7 路传感器 */
    GLAVO_Debug_Axis_Ntf,           /**< DebugAxisStatePayload：Z/S/D 位置 + Z 工作坐标 */
    GLAVO_Debug_Device_Ntf,         /**< DebugDeviceStatePayload：设备/门/电机/照明 */
    GLAVO_Debug_Galvo_Ntf,          /**< DebugGalvoStatePayload：振镜 X/Y */
    GLAVO_Debug_Laser_Ntf,          /**< DebugLaserStatePayload：激光器状态/报警/功率/温度 */
    GLAVO_Debug_Print_H264_Record_Ntf, /**< DebugOnOffPayload：打印期连续 H264 录像开关状态 */
    GLAVO_Debug_Clean_Record_Ntf,      /**< DebugOnOffPayload：清粉录像开关状态 */
    GLAVO_Debug_Env_Sim_Ntf,           /**< DebugOnOffPayload：环境上报模拟开关状态 */
    GLAVO_Debug_Env_Sim_Powder_Ntf,    /**< DebugOnOffPayload：环境模拟粉量缺粉开关状态 */
    GLAVO_Debug_FirstLayer_Visual_Check_Ntf, /**< DebugOnOffPayload：首层视觉检测开关状态 */
    GLAVO_Debug_TestImage_Ntf,         /**< DebugOnOffPayload：测试图循环打印实际状态 */

    // ---- MEDIA → UI：调试页状态回读 ----
    APP_Media_Local_Rtsp_State_Ntf, /**< DebugOnOffPayload：本地 RTSP Server 实际开关状态（相机未就绪时回 0，UI 据此弹回） */

    // ---- UI ↔ GLAVO：网络 / WiFi（ConnMan 后端） ----
    UI_Wifi_Enable_Req = 0x1300, /**< WifiEnableReqPayload：开/关 WiFi */
    UI_Wifi_Scan_Req,            /**< Empty：扫描可用网络 */
    UI_Wifi_Connect_Req,        /**< WifiConnectReqPayload：连接 SSID(+密码) */
    UI_Wifi_Forget_Req,         /**< WifiForgetReqPayload：忘记网络 */
    UI_Wifi_Info_Req,           /**< Empty：请求当前网络信息 */
    GLAVO_Wifi_State_Ntf,       /**< WifiStatePayload：连接/SSID/IP/信号（GLAVO 推送） */
    GLAVO_Wifi_Scan_Rsp,        /**< [WifiListHeader][WifiEntryPayload×N]：扫描结果 */
    GLAVO_Wifi_Saved_Rsp,       /**< [WifiListHeader][WifiEntryPayload×N]：已保存列表 */
    UI_Wifi_Saved_Req,          /**< Empty：请求已保存网络列表 → GLAVO_Wifi_Saved_Rsp */
    UI_Wifi_Disconnect_Req,     /**< WifiForgetReqPayload(复用 ssid)：断开当前连接 */
    GLAVO_Wifi_Connect_Rsp,     /**< WifiConnectRspPayload：连接结果（成功/失败），失败用于弹提示 */

    // ---- XFR → UI：WiFi 传输通知 ----
    XFR_Pair_Pin_Ntf = 0x1400,   /**< XfrPairPinPayload：屏幕显示 PIN 配对码 */
    XFR_File_Received_Ntf,       /**< XfrFileReceivedPayload：接收文件完成通知 */
    XFR_Transfer_Progress_Ntf,   /**< XfrTransferProgressPayload：接收/切片进度通知 */
    GLAVO_USB_Hotplug_Ntf,       /**< UsbHotplugPayload：USB 挂载状态变化通知 */

    // ---- UI ↔ GLAVO：清粉中断（风机停机） ----
    UI_Clean_Stop_Req = 0x1410,  /**< Empty：中断清粉并请求立即关风机 */
    APP_Clean_Stop_Rsp,          /**< Empty：风机停机请求已受理 */

    // ---- UI ↔ GLAVO：打印结果页清粉测试（按钮触发，串口 clean_test_*） ----
    UI_Clean_Test_Start_Req,     /**< CleanTestStartReqPayload：开始清粉+录像 */
    APP_Clean_Test_Start_Rsp,    /**< CleanTestStartRspPayload：是否受理 */
    APP_Clean_Test_Result_Ntf,   /**< CleanTestDonePayload：A2/42 TLV 0x03 结果 + 调试保留标记 */
    UI_Clean_Test_Close_Req,     /**< Empty：返回首页时强制 clean_test_close */
    UI_Print_Video_Finalize_Req, /**< PrintVideoFinalizeReqPayload：结果页退出后触发统一 MP4 合成 */
    APP_Clean_Test_Close_Rsp,    /**< Empty：close 已受理 */
    UI_Storage_Usage_Req,        /**< Empty：请求历史页存储占用（used/total） */
    APP_Storage_Usage_Rsp,       /**< StorageUsagePayload：存储占用回执 */

    // ---- UI ↔ GLAVO：转运盒自动转运（串口 A2/0x41） ----
    UI_Transport_Req,            /**< Empty：装盒 Ready → 发 TLV_REQ_TRANSPORT(0x02) */
    APP_Transport_Result_Ntf,    /**< TransportResultPayload：MCU TLV 0x04 结果 */

    // ---- GLAVO ↔ CAMERA：清粉连续录像会话（NV12@预览参数） ----
    APP_Clean_Video_Session_Start_Ntf, /**< CleanVideoSessionStartPayload */
    APP_Clean_Video_Session_Stop_Ntf,  /**< CleanVideoSessionStopPayload */

    // ---- VISUAL ↔ CAMERA / VISUAL → UI：视觉识别 ----
    VISUAL_Camera_Stream_Start_Req,     /**< CameraStreamStartReqPayload（视觉侧取流） */
    APP_Visual_Camera_Stream_Start_Rsp, /**< CameraStreamInfoPayload */
    VISUAL_Camera_Stream_Stop_Req,      /**< Empty */
    APP_Visual_Camera_Stream_Stop_Rsp,  /**< Empty */
    GLAVO_Visual_FirstLayer_Check_Req = 0x1423, /**< VisualFirstLayerCheckReqPayload；保持旧协议编号 */
    APP_Visual_FirstLayer_Check_Rsp = 0x1424,   /**< VisualFirstLayerCheckRspPayload；保持旧协议编号 */
    GLAVO_Visual_FirstLayer_Session_Start_Ntf = 0x1425, /**< Empty：打印开始，Visual 起流并预热定位缓存 */
    GLAVO_Visual_FirstLayer_Session_Stop_Ntf = 0x1426,  /**< Empty：首层通过/打印结束，Visual 停流休眠 */
    APP_Visual_FirstLayer_Reference_Ready_Rsp = 0x1427, /**< VisualFirstLayerReferenceReadyPayload：本次打印裸板 B0 已锁定 */
    // ---- MEDIA ↔ CAMERA：媒体进程并行取流（拍照/录像） ----
    MEDIA_Camera_Stream_Start_Req = 0x1450,  /**< CameraStreamStartReqPayload（媒体侧取流） */
    APP_Media_Camera_Stream_Start_Rsp,       /**< CameraStreamInfoPayload */
    MEDIA_Camera_Stream_Stop_Req,            /**< Empty */
    APP_Media_Camera_Stream_Stop_Rsp,        /**< Empty */

    // ---- MQTT ↔ MEDIA：云端 RTSP 推流（live；recording 一期可拒） ----
    MQTT_Media_Rtsp_Start_Req,              /**< RtspPushStartReqPayload */
    APP_Mqtt_Rtsp_Start_Rsp,                /**< RtspPushRspPayload */
    MQTT_Media_Rtsp_Stop_Req,               /**< RtspPushStopReqPayload */
    APP_Mqtt_Rtsp_Stop_Rsp,                 /**< RtspPushRspPayload */
    // ---- MQTT OTA v1.1：通知/确认/进度（下载落盘阶段） ----
    MQTT_Ota_Notify_Ntf,                    /**< OtaNotifyPayload：MQTT 侧收到 ota/notify 后转发 UI */
    UI_Ota_User_Decision_Req,               /**< OtaDecisionReqPayload：UI 确认/取消 OTA 通知 */
    APP_Mqtt_Ota_Progress_Ntf,              /**< OtaProgressPayload：下载+校验进度（OTA 应用阶段可直拉满） */

    // ---- UI ↔ GLAVO：MCU 固件版本查询（串口 PORT_GENERAL_TASK 0xA2 / CMD_PROJECT_INFO 0x10） ----
    UI_Mcu_Version_Ntf = 0x1500, /**< Empty：进入 Settings/About 页时查询一次 */
    GLAVO_Mcu_Version_Ntf,       /**< McuVersionPayload：MCU 固件版本 + 板级配置 */
    APP_Fs_Delete_Rsp,           /**< FsDeleteRspPayload：本地文件删除回执 */

    // ---- UI ↔ GLAVO：振镜控制板固件版本（以太网 TCP 连接后首包 ID=0x0F） ----
    UI_Galvo_Version_Ntf,        /**< Empty：进入 Settings/About 页时读取 Glavo 侧缓存 */
    GLAVO_Galvo_Version_Ntf,     /**< GalvoVersionPayload：TCP 0x0F 扩展包中的固件版本 */

    // ---- GLAVO → UI：设备事件/告警（消息中心） ----
    GLAVO_Device_Event_Ntf = 0x1600, /**< DeviceEventPayload：一条设备事件的触发或恢复 */

    // ---- UI ↔ GLAVO：MCU 报警（串口 PORT_ALARM_TASK 0xA3；字典见 doc/主控板串口通信协议 - 报警&事件表.csv） ----
    GLAVO_Alarm_Report_Ntf = 0x1610, /**< AlarmReportPayload：MCU 单条报警/EVENT 上报（Cmd 0x20 TLV 0x01 原样透传） */
    UI_Alarm_Reset_Ntf,              /**< AlarmResetReqPayload：请求复位某条 CONDITION 报警（Cmd 0x82 TLV 0x01） */
    GLAVO_Alarm_Snapshot_Ntf,        /**< AlarmSnapshotPayload：报警快照的一个 slot（Cmd 0x81 TLV 0x04 原样透传） */
    UI_Alarm_Snapshot_Ntf,           /**< Empty：请求 MCU 回一轮报警快照（Cmd 0x81 TLV 0x05 len=0） */
    GLAVO_Alarm_Reset_Result_Ntf,    /**< AlarmResetResultPayload：复位失败回执（Cmd 0x82 TLV 0x04） */

};

enum class MsgPriority : uint8_t {
    Low       = 0,
    Normal    = 1,
    High      = 2,
    Realtime  = 3
};

/**
 * IPC 关键消息白名单：发送失败时允许进入本地暂存并在重连后补发。
 *
 * 说明：
 * 1) 仅放“状态终值/关键事件”与打印链路关键里程碑，避免把高频实时流量全量缓存。
 * 2) 高频进度仍建议业务侧做“只认最新状态”去旧保护。
 */
inline constexpr bool is_ipc_reliable_msg_type(MsgType type) noexcept
{
    switch(type) {
    // OTA / 告警：终态与关键事件
    case MsgType::APP_Mcu_Ota_Status_Ntf:
    case MsgType::APP_Mqtt_Ota_Progress_Ntf:
    case MsgType::GLAVO_Alarm_Report_Ntf:
    case MsgType::GLAVO_Alarm_Reset_Result_Ntf:
        return true;

    // 打印主流程：关键状态与异常通知
    case MsgType::APP_Print_Progress_Ntf:
    case MsgType::APP_Print_Terminate_Rsp:
    case MsgType::APP_Print_Pause_Rsp:
    case MsgType::APP_Print_Powder_Low_Ntf:
        return true;

    // 换层/录像链路：层抓拍与会话闭环
    case MsgType::APP_Print_Video_Session_Start_Ntf:
    case MsgType::APP_Print_Layer_Snap_Ntf:
    case MsgType::APP_Print_Video_Session_Finish_Ntf:
    case MsgType::APP_Print_Video_Session_Abort_Ntf:
    case MsgType::APP_Print_Video_Manifest_Ntf:
        return true;

    // 首层视觉关键里程碑
    case MsgType::APP_Visual_FirstLayer_Check_Rsp:
    case MsgType::APP_Visual_FirstLayer_Reference_Ready_Rsp:
    case MsgType::GLAVO_Visual_FirstLayer_Session_Start_Ntf:
    case MsgType::GLAVO_Visual_FirstLayer_Session_Stop_Ntf:
        return true;

    default:
        return false;
    }
}

/**
 * IPC 状态流消息：在本地补发队列中仅保留“最新一条”。
 *
 * 用于避免断连期间高频状态堆积（旧状态补发导致回跳/队列膨胀）。
 */
inline constexpr bool is_ipc_latest_only_msg_type(MsgType type) noexcept
{
    switch(type) {
    case MsgType::APP_Mqtt_Ota_Progress_Ntf:
    case MsgType::APP_Mcu_Ota_Status_Ntf:
    case MsgType::APP_Print_Progress_Ntf:
    case MsgType::APP_Print_Pause_Rsp:
    case MsgType::APP_Print_Video_Manifest_Ntf:
    case MsgType::APP_Visual_FirstLayer_Reference_Ready_Rsp:
        return true;
    default:
        return false;
    }
}

// Payload 契约（POD，可 memcpy；跨进程由 ipc_wire 序列化）
// 命名：\<动作\>Payload / \<动作\>ReqPayload / \<动作\>RspPayload

enum class UserHomeStatus : uint32_t {
    Idle     = 0,
    Ready    = 1,
    Printing = 2,
    Error    = 3,
};

/** APP_Update_Ntf 载荷：首页环境/状态快照（Inline）
 *  尾部扩展字段为首页/遥测复用指标；后端未采集到时填 <0（或 NaN）表示无效，
 *  UI 首页据此显示 "--"（见 page_user_home 哨兵判空）。 */
struct EnvSnapshotPayload {
    char     device_name[32];
    uint32_t status;            /**< UserHomeStatus */
    uint8_t  wifi_connected;
    int32_t  signal_bars;       /**< 信号格数 0..3（图标三格） */
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
    float    pm1_indoor;        /**< 室内 PM1.0 (μg/m³)；无效填 <0 */
    float    pm10_indoor;       /**< 室内 PM10 (μg/m³)；无效填 <0（HOME 卡切换指标用） */
    float    humidity_percent;  /**< 舱内湿度 %；无效填 <0 */
    float    pressure_kpa;      /**< 舱内气压 kPa；无效填 <0 */
    float    baseplate_temp_c;  /**< 基板温度 ℃；无效填 <0 */
    uint8_t  powder_bits;       /**< 粉量位：取 bit0/bit1（0=缺粉；>=1 允许继续打印） */
};

/** APP_PM25_Rsp 载荷：PM2.5 检测结果（Inline） */
struct Pm25ResultPayload {
    uint8_t is_high;
    float   pm25_cabin;
};

// ---- 网络 / WiFi 载荷 ----
enum class WifiSecurity : uint32_t { Open = 0, WPA2 = 1, WPA3 = 2 };

/** UI_Wifi_Enable_Req：开/关 WiFi（Inline） */
struct WifiEnableReqPayload {
    uint8_t enable;   /**< 1=开 0=关 */
};

/** UI_Wifi_Connect_Req：连接指定网络（Inline） */
struct WifiConnectReqPayload {
    char ssid[64];
    char password[64];
};

/** UI_Wifi_Forget_Req：忘记网络（Inline） */
struct WifiForgetReqPayload {
    char ssid[64];
};

/** GLAVO_Wifi_State_Ntf：当前网络状态（Inline）——首页图标 + 设置详情共用 */
struct WifiStatePayload {
    uint8_t  enabled;       /**< WiFi 开关是否开 */
    uint8_t  connected;     /**< 是否已连接 */
    int32_t  signal_bars;   /**< 信号格数 0..3（图标三格） */
    char     ssid[64];      /**< 当前 SSID，未连为空 */
    char     ip_addr[46];   /**< 当前 IP 文本，未连为空 */
};

/** GLAVO_Wifi_Scan_Rsp / GLAVO_Wifi_Saved_Rsp BufferRef 布局：[WifiListHeader][WifiEntryPayload × count] */
struct WifiListHeader {
    uint32_t count;
};

/** 单个 WiFi 条目（扫描 / 已保存共用） */
struct WifiEntryPayload {
    char     ssid[64];
    int32_t  signal_bars;   /**< 0..3（图标三格） */
    uint32_t security;      /**< WifiSecurity */
    uint8_t  saved;         /**< 是否已保存 */
    uint8_t  connected;     /**< 是否当前连接 */
};

/** GLAVO_Wifi_Connect_Rsp：连接结果（Inline）。ok=0 时 UI 弹"连接失败"提示 */
struct WifiConnectRspPayload {
    uint8_t ok;             /**< 1=成功 0=失败(如密码错/超时) */
    char    ssid[64];
};

/** XFR_Pair_Pin_Ntf：XFR 通知 UI 显示 PIN（Inline） */
struct XfrPairPinPayload {
    char     pin[8];       /**< 4 位数字 PIN，字符串表示 */
    uint32_t expires_s;    /**< PIN 剩余有效期（秒） */
};

/** XFR_File_Received_Ntf：XFR 通知 UI 刷新/提示（Inline） */
struct XfrFileReceivedPayload {
    char     file_name[128];
    uint64_t file_size;
};

/** XFR_Transfer_Progress_Ntf：XFR 传输阶段进度（Inline） */
enum class XfrTransferState : uint8_t {
    Idle      = 0,  /**< 无活跃传输 */
    Uploading = 1,  /**< HTTP 分块上传中 */
    Slicing   = 2,  /**< 上传完成后切片中 */
    Finished  = 3,  /**< 切片完成并落盘 */
    Error     = 4,  /**< 传输或切片失败 */
};

struct XfrTransferProgressPayload {
    char     file_name[128];      /**< 当前任务名（切片阶段优先展示目标 gcode） */
    uint8_t  state;               /**< XfrTransferState */
    uint8_t  reserved[3];
    uint32_t progress_percent;    /**< 0..100 */
    uint64_t uploaded_bytes;      /**< 上传阶段已接收字节 */
    uint64_t total_bytes;         /**< 上传阶段总字节 */
};

/** GLAVO_USB_Hotplug_Ntf：USB 存储挂载状态变化（Inline） */
struct UsbHotplugPayload {
    uint8_t present;         /**< 1=检测到可用 USB 存储；0=未检测到 */
    uint8_t mounted;         /**< 1=挂载可读；0=未挂载 */
    uint8_t reserved[2];
    char    mount_path[64];  /**< 约定挂载路径（如 /media/udisk0） */
};

/** APP_CLOSE_DOOR_Rsp / APP_OpenDoor_Rsp 载荷：动作结果（Inline） */
struct DoorResultPayload {
    uint8_t ok;
};

/** 打印文件来源（DirOpen 仅 USB；Prepare / Start 共用；Local 列表由 UI 读 config.json） */
enum class PrintFileSource : uint8_t {
    Local = 0,
    Usb   = 1,
};

/** Fs_DirOpen_Req：仅 USB；路径由 Glavo device_config 配置（Local 不走此消息） */
struct FsDirOpenReqPayload {
    PrintFileSource list_source; /**< 须为 Usb */
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

/** UI_Print_Prepare_Req：按 Tab 来源 + 索引 */
struct PrintPrepareReqPayload {
    uint32_t        file_index;
    PrintFileSource file_source;
    uint8_t         reserved[3];
};

struct PrintPrepareRspPayload {
    int8_t ok;   /**< 1=成功，0=失败 */
    uint8_t reserved[3];
};

/** UI_Print_Start_Req：来源 + 索引 */
struct PrintStartReqPayload {
    uint32_t        file_index;
    PrintFileSource file_source;
    uint8_t         reserved[3];
};

/** Fs_Delete_Ntf 载荷：文件来源 + 索引（Local 列表删除） */
using FsDeleteNtfPayload = PrintPrepareReqPayload;

/** APP_Fs_Delete_Rsp：本地删除结果（UI 收到 ok=1 后再刷新 Local 目录）。 */
enum class FsDeleteResult : uint8_t {
    Ok = 0,
    InvalidPayload = 1,
    OnlyLocalSupported = 2,
    BusyPrinting = 3,
    IndexOutOfRange = 4,
    ManifestUpdateFailed = 5,
};

struct FsDeleteRspPayload {
    uint32_t file_index;        /**< 回显请求索引 */
    uint8_t  file_source;       /**< PrintFileSource */
    uint8_t  ok;                /**< 1=删除并落盘成功 0=失败 */
    uint8_t  reason;            /**< FsDeleteResult */
    uint8_t  reserved;
};

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
    uint16_t cmd_cache_rate;   /**< MCU 指令缓存占用率（TCP 心跳，0=排空） */
    uint16_t data_cache_rate;  /**< MCU 数据缓存占用率（TCP 心跳，0=排空） */
};

/** GLAVO_Print_State_Ntf 载荷：开始打印/打印结束状态边沿（Inline）。
 *  mode 取值复用 UserHomeStatus：Printing=2 表示开始打印，Idle=0 表示打印结束。
 *  since_ms 为进入该 mode 的 Unix 毫秒时间戳（Glavo 侧填写）。 */
struct PrintStateNtfPayload {
    uint32_t mode;       /**< UserHomeStatus：2=Printing 开始打印 / 0=Idle 打印结束 */
    uint32_t reserved;
    uint64_t since_ms;   /**< 进入当前 mode 的 Unix 毫秒时间戳 */
};

/** UI_Camera_Stream_Start_Req：可选 basename；空串则使用后端当前相机实时流 */
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

/** APP_Camera_Snapshot_Rsp：保存当前帧结果（Inline） */
struct CameraSnapshotRspPayload {
    int8_t ok;             /**< 1=成功，0=失败 */
    uint8_t reserved[3];
    char path[128];        /**< 成功时输出路径；失败为空串 */
};

/** GLAVO_Visual_FirstLayer_Check_Req：触发首层视觉检测（Inline） */
struct VisualFirstLayerCheckReqPayload {
    uint16_t layer_height_um; /**< 当前首层高度（um） */
    uint8_t reserved[2];
};

/** APP_Visual_FirstLayer_Check_Rsp：首层视觉检测结果（Inline） */
struct VisualFirstLayerCheckRspPayload {
    uint8_t result;      /**< 1=通过（可出光），0=不通过（继续铺粉） */
    uint8_t reserved[3];
};

/** APP_Visual_FirstLayer_Reference_Ready_Rsp：同会话裸板 B0 采集结果 */
struct VisualFirstLayerReferenceReadyPayload {
    uint8_t result;      /**< 1=B0 已采集且可用，0=采集失败；失败时禁止开始运动 */
    uint8_t reserved[3];
    uint32_t frame_seq;  /**< B0 对应相机帧序号；失败时为 0 */
};

/** GLAVO→CAMERA 打印录像会话（Start/Finish/Abort 共用） */
struct PrintVideoSessionPayload {
    char     session_id[32];
    char     job_name[64];   /**< 用于输出 /data/media/video/{job}/ 目录名 */
    uint32_t width;          /**< 0=Camera 默认 */
    uint32_t height;         /**< 0=Camera 默认 */
    uint32_t layer_fps;      /**< MP4 层帧率；0=默认 */
    uint8_t  h264_continuous_on; /**< 1=本次打印会话启用连续 H264 录制 */
    uint8_t  reserved[3];
    uint32_t h264_bitrate;   /**< 连续 H264 CBR 码率（bps）；0=Media 默认（cal PRINT_H264_BITRATE / 中等） */
};

/** GLAVO→CAMERA 单层拍照（换层排空后、LAYER_DONE 前） */
struct PrintVideoLayerSnapPayload {
    char     session_id[32];
    uint32_t layer_index;    /**< 1-based */
    uint16_t layer_height_um;
    uint16_t reserved;
};

/**
 * APP_Print_Video_Manifest_Ntf：Media→UI，打印历史清单状态通知（Inline）
 *
 * 发送时机：`PrintVideoRecorder::finalize_session()` 内至少发送一次。
 * - 快路径：统计/缩略图已落盘时先发（`has_video=0`），UI 可立即切结果页
 * - 慢路径：MP4 完成后再发（`has_video=1`）用于历史页刷新可播放状态
 *
 * 0 帧场景（未触发换层 Snap / Camera 未采到帧）不会写清单，但**仍会发本消息**
 * （`has_entry=0`），否则 UI 会永远停在打印中界面下不来。
 *
 * UI 用法：
 *   - `has_entry=1` → 以 `frames_dir` 为主键在 `/data/media/video/config.json` 定位该条，
 *     读 `status` / `duration_sec` / `material_g`（清单为权威数据源）
 *   - `has_entry=0` → 清单无此条，改用本 payload 的 `aborted` 判定完成/终止，
 *     耗时与耗材显示占位
 */
struct PrintVideoManifestPayload {
    char    frames_dir[64];  /**< 清单主键（相对目录名）；has_entry=0 时为空串 */
    int8_t  has_entry;       /**< 1=config.json 已写入该条；0=无帧未落盘 */
    int8_t  aborted;         /**< 1=终止 0=完成；仅 has_entry=0 时作为判定依据 */
    int8_t  has_video;       /**< 1=timelapse.mp4 已生成 */
    uint8_t reserved;
};

/** UI_Clean_Test_Start_Req：Purify / 手动清粉触发清粉测试（录像可选）
 *
 * `record_video=0` 时 Glavo 不开 Media 清粉录像会话，因此不写
 * `/data/media/video/{job_name}/`、也不进打印历史；MCU 清粉结束后由 Glavo 自己发
 * `APP_Clean_Test_Result_Ntf`（`on_clean_test_done()` 的 `!video_started` 分支）。
 * 控制页的手动清粉走这一档，`job_name` 随之无人读取。 */
struct CleanTestStartReqPayload {
    char job_name[64];   /**< 对应 /data/media/video/{job_name}/；record_video=0 时忽略 */
    uint32_t width;      /**< 0=跟随 Camera 当前预览宽 */
    uint32_t height;     /**< 0=跟随 Camera 当前预览高 */
    uint32_t fps;        /**< 0=跟随 Camera 当前预览 fps */
    uint8_t record_video; /**< 1=同时开清粉录像（结果页 Purify）；0=只清粉不录 */
    uint8_t reserved[3];
};

/** APP_Clean_Test_Start_Rsp：清粉测试请求是否被 Glavo 受理 */
struct CleanTestStartRspPayload {
    int8_t accepted;     /**< 1=已受理 0=拒绝（如已有进行中的清粉测试） */
    uint8_t reserved[3];
};

/** APP_Clean_Test_Result_Ntf：清粉完成结果通知。
 * 约定：result_code 为结束类型，00/01/02 均表示 MCU 清粉流程已结束；
 * 有清粉录像时由 Media 在清粉抽帧停止后发送；最终 MP4 在结果页退出后统一合成。 */
struct CleanTestDonePayload {
    uint8_t result_code;   /**< 结束类型：00/01/02 都表示清粉结束 */
    uint8_t raw_retained;  /**< 兼容字段；PNG 路径下恒为 0 */
    uint8_t has_mp4;       /**< 1=期望存在合并后的 timelapse.mp4 */
    uint8_t reserved;
    char    mp4_rel_path[96]; /**< 相对 /data/media/video 的路径，如 job/timelapse.mp4 */
};

/** UI→MEDIA：结果页退出后触发统一 MP4 合成（按 frames_dir 定位会话目录） */
struct PrintVideoFinalizeReqPayload {
    char frames_dir[96]; /**< 对应 /data/media/video/{frames_dir}/ */
};

/** APP_Storage_Usage_Rsp：历史页容量显示（bytes）。 */
struct StorageUsagePayload {
    uint64_t used_bytes;   /**< 已用字节数 */
    uint64_t total_bytes;  /**< 总字节数 */
};

/** APP_Transport_Result_Ntf：自动转运流程结果（串口 TLV 0x04）。 */
struct TransportResultPayload {
    uint8_t result;      /**< 0=完成；1=拒绝或失败；2=已中止 */
    uint8_t reserved[3];
};

/** GLAVO→MEDIA：清粉延时抽帧会话开始（接续打印 PNG，按 fps 定时采样） */
struct CleanVideoSessionStartPayload {
    char     session_id[32];
    char     job_name[64];
    uint32_t width;      /**< 0=沿用 Camera 预览宽 */
    uint32_t height;     /**< 0=沿用 Camera 预览高 */
    uint32_t fps;        /**< 抽帧/合成帧率；0=Media 默认（cal TIMELAPSE_VIDEO_FPS） */
};

/** GLAVO→MEDIA：清粉延时抽帧会话停止 */
struct CleanVideoSessionStopPayload {
    char    session_id[32];
    uint8_t result_code;  /**< 结束类型：00/01/02=MCU清粉结束；0xfe=用户关闭；0xff=内部收尾 */
    uint8_t keep_raw;     /**< 兼容字段；PNG 路径下忽略 */
    uint8_t reserved[2];
};

/**
 * MQTT→MEDIA：开始 RTSP 推流（对接说明：push_url 已含 ?token=，设备原样使用）。
 * result 回执见 RtspPushRspPayload。
 */
struct RtspPushStartReqPayload {
    char     req_id[64];
    char     push_url[512];       /**< 完整 ANNOUNCE URL（可含 token query） */
    char     push_token[256];     /**< 冗余字段；一期忽略，以 push_url 为准 */
    char     stream_type[16];     /**< "live" / "recording" */
    char     recording_id[64];    /**< recording 时用；一期可不填 */
    uint32_t seek_s;              /**< recording seek；live 忽略 */
    uint32_t max_bitrate_kbps;    /**< 0=Media 默认 */
    uint8_t  transport;           /**< 0=tcp（协议固定） */
    uint8_t  reserved[3];
};

/** MQTT→MEDIA：停止 RTSP 推流（按 stream_type 停对应会话） */
struct RtspPushStopReqPayload {
    char req_id[64];
    char stream_type[16]; /**< "live" / "recording" */
};

/** MEDIA→MQTT：开/停推流结果（映射 MQTT command_ack） */
enum class RtspPushResult : uint8_t {
    Accepted = 0,
    Rejected = 1,
    Error    = 2,
};

struct RtspPushRspPayload {
    char     req_id[64];
    char     stream_type[16];
    char     reason[32];          /**< rejected/error 时：push_already_active / encoder_error / … */
    uint8_t  result;              /**< RtspPushResult */
    uint8_t  reserved[3];
    uint64_t push_started_at;     /**< 毫秒；accepted 时填写，否则 0 */
};

/** APP_Mcu_Ota_Status_Ntf / APP_Serial_Mcu_Ota_Status_Ntf：OTA 进度/状态 */
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

/** MQTT_Ota_Notify_Ntf：云端 ota_notify 下发后的 UI 提醒载荷 */
enum class OtaApprovalStatus : uint8_t {
    PendingApproval = 0,  /**< PENDING_APPROVAL */
    Approved        = 1,  /**< APPROVED */
    Ready           = 2,  /**< READY（required=true） */
    Unknown         = 255
};

struct OtaNotifyPayload {
    char    task_id[64];
    char    release_version[32];
    uint8_t required;           /**< 1=true（强制） 0=false */
    uint8_t approval_status;    /**< OtaApprovalStatus */
    uint8_t reserved[2];
};

/** UI_Ota_User_Decision_Req：用户对 ota_notify 的确认/取消 */
struct OtaDecisionReqPayload {
    char    task_id[64];
    uint8_t approve;            /**< 1=确认并继续 manifest 请求；0=取消 */
    uint8_t reserved[3];
};

/** APP_Mqtt_Ota_Progress_Ntf：下载+校验阶段进度（百分比制） */
enum class OtaProgressState : uint8_t {
    Downloading = 0,
    Verifying   = 1,
    Applying    = 2,  /**< 当前阶段可跳过真实 OTA，直接拉满 */
    Completed   = 3,
    Failed      = 4,
    RolledBack  = 5
};

struct OtaProgressPayload {
    char    task_id[64];
    char    component[16];      /**< soc / mcu / galvo */
    uint8_t state;              /**< OtaProgressState */
    uint8_t progress_pct;       /**< 0..100 */
    uint8_t reserved[2];
    char    error_code[64];     /**< 失败原因码；成功/进行中为空串 */
};

/**
 * GLAVO_Mcu_Version_Ntf：MCU 固件版本与编译期板级配置（串口 A2/0x10 TLV 0x02 上报）
 * About 页按 V<major>.<minor>.<patch>.<laser_type>.<z_motor_type> 显示。
 */
struct McuVersionPayload {
    uint8_t valid;        /**< 1=字段有效；0=未取到 */
    uint8_t major;        /**< 大版本 0~255 */
    uint8_t minor;        /**< 子版本 0~255 */
    uint8_t patch;        /**< 修订号 0~255 */
    uint8_t laser_type;   /**< 激光器类别：JFSC=1，AL=2 */
    uint8_t z_motor_type; /**< Z 轴电机：闭环 BRIDGE=0，开环 OPEN_CONTROL=1 */
    uint8_t reserved[2];
};

/**
 * GLAVO_Galvo_Version_Ntf：振镜控制板固件版本（TCP 连接后首包 ID=0x0F，Length≥0x08）
 * 版本字为小端 uint32；About 页按 V<(v>>16)&0xff>.<(v>>8)&0xff>.<(v&0xff)> 显示
 *（如 0x00030004 → V3.0.4）。旧固件仅发 Length=0x01 就绪包时 valid=0。
 */
struct GalvoVersionPayload {
    uint8_t  valid;          /**< 1=已收到过带版本的 TCP 0x0F 扩展包 */
    uint8_t  firmware_id;    /**< 1=APPA，2=APPB（协议表） */
    uint8_t  reserved[2];
    uint32_t fw_version_le;  /**< 小端固件版本号（与 TCP 帧 byte[6..9] 一致） */
};

// 调试页（手动控制/标定）契约 —— 命名见 common/COMMON.md；枚举跨进程时按 uint8_t 传输

enum class DebugAxis        : uint8_t { Z = 0, S = 1, D = 2 };
enum class DebugJogMode     : uint8_t { Click = 0, Hold = 1 };
enum class DebugGalvoAxis   : uint8_t { X = 0, Y = 1 };
enum class DebugDeviceTarget: uint8_t { Device = 0, Door = 1, Motor = 2, Light = 3 };
enum class DebugMotorAction : uint8_t { Stop = 0, Forward = 1, Reverse = 2 };
enum class DebugSwitchTarget: uint8_t { Laser = 0, GasWash = 1, BaseHeat = 2 };
enum class DebugPowderStatus: uint8_t { Available = 0, Busy = 1, Fault = 2 };

/** UI_Debug_Axis_Jog_Ntf：轴点动 / 长按点动 */
struct DebugAxisJogPayload {
    uint8_t axis;       /**< DebugAxis */
    uint8_t mode;       /**< DebugJogMode：Click=单步，Hold=持续 */
    int8_t  dir;        /**< +1 / -1 */
    uint8_t reserved;
    float   value;      /**< Click=步距(mm/°)，Hold=速度(mm·°/s) */
};

/** UI_Debug_Axis_Hold_Stop_Ntf / UI_Debug_Axis_SetZero_Ntf */
struct DebugAxisPayload {
    uint8_t axis;       /**< DebugAxis */
    uint8_t reserved[3];
};

/** UI_Debug_Galvo_Move_Ntf：振镜方向步进 */
struct DebugGalvoMovePayload {
    uint8_t axis;       /**< DebugGalvoAxis */
    int8_t  dir;        /**< +1 / -1 */
    uint8_t reserved[2];
    float   step_mm;
};

/** 通用开关：UI_Debug_Galvo_RedLight_Ntf / UI_Debug_Pusher_Ntf */
struct DebugOnOffPayload {
    uint8_t on;
    uint8_t reserved[3];
};

/** UI_Debug_Galvo_Param_Save_Ntf：写入 cal.csv（Scale 可保留；中心偏移写第 4 行） */
struct DebugGalvoParamPayload {
    float scale;
    float step_mm;          /**< 调试页 Step 选择，后端可忽略 */
    float center_offset_x;  /**< cal.csv 第 4 行 X 中心平移（mm） */
    float center_offset_y;  /**< cal.csv 第 4 行 Y 中心平移（mm） */
};

/** UI_Debug_Device_Ntf：设备/门/电机/照明 */
struct DebugDevicePayload {
    uint8_t target;     /**< DebugDeviceTarget */
    uint8_t on;
    uint8_t reserved[2];
};

/** UI_Debug_Powder_Bucket_Ntf：粉桶开关 */
struct DebugBucketPayload {
    uint8_t bucket_id;  /**< 1 / 2 */
    uint8_t on;
    uint8_t reserved[2];
};

/** UI_Debug_Recycle_Motor_Ntf：回粉电机 */
struct DebugMotorPayload {
    uint8_t action;     /**< DebugMotorAction */
    uint8_t reserved[3];
};

/** UI_Debug_Fan_Ntf：风扇开关 + 占空比 */
struct DebugFanPayload {
    uint8_t on;
    uint8_t reserved[3];
    float   duty;       /**< 0..100 (%) */
};

/** UI_Debug_Switch_Ntf：激光/洗气/底板加热 */
struct DebugSwitchPayload {
    uint8_t target;     /**< DebugSwitchTarget */
    uint8_t on;
    uint8_t reserved[2];
};

/** UI_Debug_PrintParam_Set_Ntf：打印层参数（层厚 + 平衡系数×1000） */
struct DebugPrintParamPayload {
    uint16_t layer_um;        /**< 层厚 um */
    uint16_t balance_x1000;   /**< 平衡系数 × 1000 */
};

/** UI_Debug_Laser_Power_Set_Ntf：激光手动功率百分比 */
struct DebugLaserPowerPayload {
    uint8_t percent;          /**< 0..100 */
    uint8_t reserved[3];
};

/** GLAVO_Debug_Laser_Ntf：激光器状态上报（原始值，界面自行换算） */
struct DebugLaserStatePayload {
    uint16_t status;          /**< 状态 bitfield，bit 含义随 laser_type 变化（见激光协议文档 1.1/2.2） */
    uint16_t alarm;           /**< laser_alarm 打包报警位，bit 含义随 laser_type 变化（见 1.2/2.3） */
    uint16_t power_raw;       /**< 功率原始值（JFSC 0..255；AL 单位 0.1%） */
    uint16_t pump_temp_raw;   /**< 温度原始值（JFSC 整数℃；AL 单位 0.1℃） */
    uint16_t current_raw;     /**< 电流原始值（JFSC 单位 0.01A；AL 单位 0.1A；仅 3ST 上报，2ST 为 0） */
    uint8_t  laser_type;      /**< 激光器型号：0=未知 1=JFSC 2=AL 200W（2ST 无型号，回退最近一次 3ST 值） */
    uint8_t  reserved[3];
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

/** GLAVO_Debug_Galvo_Ntf：振镜 X/Y */
struct DebugGalvoStatePayload {
    float   x;
    float   y;
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

/* ============================================================================
 *  设备事件 / 告警（消息中心）
 *
 *  码表与 doc/设备通信协议-数据结构定义（v0.9）.md「五、事件与告警消息」那张表
 *  一一对应，云端用字符串 code，这里换成数值以便走 Inline POD。
 *  文案不进线协议：后端只发码，UI 侧持一张码→文案表（见 ui_message_store）。
 * ========================================================================== */

/** 事件码。数值跨进程传输，**只准往末尾追加**，不得插入或复用。 */
enum class DeviceEventCode : uint16_t {
    None                = 0,
    /* severity=info */
    PowderLow           = 1,   /**< 低粉位传感器触发 */
    FilterDue           = 2,   /**< 滤芯使用时长/压差达阈值 */
    PrintPaused         = 3,   /**< 用户主动暂停 */
    PrintComplete       = 4,   /**< 所有层打印完成（one_shot） */
    PurgeComplete       = 5,   /**< O2 降至阈值，洗气完成（one_shot） */
    /* severity=error */
    WeightSensorErr     = 6,   /**< HX711 读数异常 */
    GalvoCalFail        = 7,   /**< 振镜校准失败 */
    MotionStallguard    = 8,   /**< TMC5160 StallGuard 触发，运动轴失步 */
    TempSensorErr       = 9,   /**< NTC/IR 传感器读数无效 */
    NtpSyncFail         = 10,  /**< NTP 对时失败超阈值 */
    DoorSensorConflict  = 11,  /**< D2F 与霍尔读数不一致 */
    JobFileErr          = 12,  /**< 作业文件下载失败或校验不通过（one_shot） */
    PowderInsufficient  = 13,  /**< 粉料估算不足以完成当前作业（one_shot） */
    GalvoNotCalibrated  = 14,  /**< 振镜未校准或校准过期 */
    PrintError          = 15,  /**< 打印过程不可恢复错误（one_shot） */
    /* severity=critical */
    DoorOpen            = 16,  /**< 打印/洗气中外门未关 */
    O2High              = 17,  /**< 氧含量超阈值 */
    Pm25High            = 18,  /**< PM2.5 超阈值 */
    Overtemp            = 19,  /**< 仓内温度超阈值 */
    LaserInterlock      = 20,  /**< 激光安全联锁触发 */
    FilterOvertemp      = 21,  /**< 滤芯 NTC 超温 */
    FilterRequired      = 22,  /**< 滤芯严重超限，强制阻断打印 */
    /* severity=emergency */
    Smoke               = 23,  /**< 烟感触发 */
    Estop               = 24,  /**< 急停双触点触发 */
    FireSuppress        = 25,  /**< 灭火阀动作 */
    Esd                 = 26,  /**< 静电保护触发 */
    FirstLayerVisualRecoat = 27, /**< 首层检测未通过，继续铺粉重检（one_shot） */
    FirstLayerVisualPassed = 28, /**< 首层检测通过，进入正常打印（one_shot） */
    FirstLayerReferenceRetrying = 29, /**< 参考图采集失败，继续重试（one_shot） */
    FirstLayerReferenceReady = 30, /**< 参考图采集成功，开始首层检测（one_shot） */

    /* ---- 以下为 UI 本地事件：不走总线，由前端自己 push，占同一码空间以便
     *      消息中心统一排序去重。后端不应发送这些码。 ---- */
    UiWearMask          = 512, /**< 吹粉完成，开门前提醒戴口罩 */
    UiPurifyBeforeOpen  = 513, /**< 打印结束，开门前先吹粉 */
    UiDebugSample       = 514, /**< 调试页注入的假消息 */
};

/** 事件等级。UI 只分两种观感：Info 走蓝色可关卡片，其余走红色告警卡片。 */
enum class DeviceEventSeverity : uint8_t {
    Info      = 0,
    Error     = 1,
    Critical  = 2,
    Emergency = 3,
};

/** GLAVO_Device_Event_Ntf 载荷：一条事件的触发或恢复（Inline）。
 *
 *  event_ts_ms 是告警**实际触发/恢复**时刻，不是消息发出时刻——断网补发或
 *  NTP 失步恢复后两者会差很多，消息中心的时间线以本字段为准（见协议文档同节说明）。 */
struct DeviceEventPayload {
    uint16_t code;         /**< DeviceEventCode */
    uint8_t  severity;     /**< DeviceEventSeverity */
    uint8_t  active;       /**< stateful：1=触发 0=恢复；one_shot 恒为 1 */
    int64_t  event_ts_ms;  /**< 触发/恢复时刻（Unix 毫秒），0=未知 */
};

/** AlarmReportPayload.state_mask 位定义（bit3..7 保留，解析时忽略）。 */
constexpr uint8_t kAlarmStateActive  = 0x01; /**< 故障条件当前存在 */
constexpr uint8_t kAlarmStateLatched = 0x02; /**< 已锁存，等待清除 */
constexpr uint8_t kAlarmStateEvent   = 0x04; /**< 一次性 EVENT，无生命周期 */

/** AlarmReportPayload.clear_mode 枚举。 */
enum class AlarmClearMode : uint8_t {
    Auto       = 0,  /**< 条件消失即自动恢复 NORMAL */
    Reset      = 1,  /**< 需 UI 复位请求（UI_Alarm_Reset_Ntf） */
    PowerCycle = 2,  /**< 断电重启才能清除 */
    None       = 3,  /**< 不可清除（本次上电周期内一直存在） */
};

/** GLAVO_Alarm_Report_Ntf 载荷：MCU 单条报警上报（串口 0xA3/0x20 TLV 0x01，len=9）。
 *
 *  五个字段与 TLV Value 一一对应、Glavo 侧不加工不查表：severity/clear_mode
 *  一律以本报文为准（报警字典可能落后于 MCU 固件，见
 *  doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md §1）。生命周期由 UI 侧
 *  按 state_mask 推导：bit2=EVENT（一次性消息）；否则 active/latched 组合出
 *  ACTIVE / RTN_LATCHED / NORMAL 三态。 */
struct AlarmReportPayload {
    uint16_t alarm_code;   /**< 稳定报警号（字典主键，RESET 也用它） */
    uint8_t  severity;     /**< 1=information 2=warning 3=error 4=critical；未知值按不低于 warning 显示 */
    uint8_t  state_mask;   /**< bit0=active bit1=latched bit2=EVENT，bit3..7 保留 */
    uint8_t  clear_mode;   /**< 0=AUTO 1=RESET 2=POWER_CYCLE 3=NONE；未知值按不可软件复位处理 */
    uint8_t  reserved[3];
    uint32_t detail;       /**< 详情号/原因值，原样保留；0=无详情 */
};

/** 快照 slot 总数：4 个报警组 × 每组 3 个 mask 字。`slot_id` 合法范围 0..11。 */
constexpr uint8_t kAlarmSnapshotSlotCount = 12;

/**
 * 快照槽位 → 报警号的固定公式（`doc/SOC_TLV_ALARM_EVT_SNAPSHOT报警快照开发指导.md` §6）。
 *
 * 报警码按百位分 4 组，每组预留 100 个号但只有前 96 个进快照（`xx96..xx99`
 * 永久保留）。组内 96 个号拆成 3 个 32 bit 的 mask 字，`slot_id = group*3 + word`。
 *
 * 这是**纯计算**，不查表：CSV 里的 slot/bit 列只是显示元数据，字典比固件旧时
 * 也必须能算出报警号（该文档 §6.5 明令禁止改用内部 index 推算）。
 */
constexpr uint16_t alarm_snapshot_code_of(uint8_t slot_id, uint8_t bit)
{
    return static_cast<uint16_t>(1000 + (slot_id / 3) * 100 + (slot_id % 3) * 32 + bit);
}

/** 报警号是否落在快照管辖范围内（1000..1395，且组内偏移 < 96）。 */
constexpr bool alarm_snapshot_covers(uint16_t code)
{
    return code >= 1000 && code <= 1395 && ((code - 1000) % 100) < 96;
}

/** GLAVO_Alarm_Snapshot_Ntf 载荷：报警快照的**一个 slot**（串口 0xA3/0x81 TLV 0x04，len=11）。
 *
 *  快照是**请求-响应**的：MCU 不会主动推，UI 得先发 UI_Alarm_Snapshot_Ntf。
 *  响应**稀疏**——只回 (active|latched) 非零的 slot，没回的 slot 一律按全零处理，
 *  不能保留上一轮旧值。`nonzero_slot_count=0` 是合法的「当前无报警」响应。
 *  Glavo 侧不加工，攒帧与原子对齐都在 UI（`ui_alarm_state`）做。
 *
 *  只含 CONDITION，EVENT 不占 bit。槽位→报警号用 alarm_snapshot_code_of()。 */
struct AlarmSnapshotPayload {
    uint8_t  nonzero_slot_count; /**< 本轮非零 slot 总数（0..12）；0=全清 */
    uint8_t  slot_id;            /**< 本帧的 slot（0..11），非连续、不保证顺序 */
    uint8_t  highest_severity;   /**< 快照时刻最高等级；0=无有效 CONDITION */
    uint8_t  reserved;
    uint32_t active_mask;        /**< 本 slot 32 条的 active 位 */
    uint32_t latched_mask;       /**< 本 slot 32 条的 latched 位 */
};

/** UI_Alarm_Reset_Ntf 载荷：请求复位某条报警（串口 0xA3/0x82 TLV 0x01）。
 *  仅对 clear_mode=RESET 的 CONDITION 有意义；通信 ACK 不代表复位成功，最终
 *  状态以后续 GLAVO_Alarm_Report_Ntf 为准（失败会另有 Reset_Result 回执）。 */
struct AlarmResetReqPayload {
    uint16_t alarm_code;
    uint8_t  reserved[2];
};

/** AlarmResetResultPayload.result 取值（MCU 只在失败时回执）。 */
constexpr uint8_t kAlarmResetResultFailed = 0x01;

/** GLAVO_Alarm_Reset_Result_Ntf 载荷：复位**失败**回执（串口 0xA3/0x82 TLV 0x04，len=3）。
 *
 *  只在失败时发，成功仍以后续 NORMAL 单报为准。串口 Seq 固定 0，所以按
 *  alarm_code 关联在途请求、不按 Seq。UI 收到即解除该条的 reset_pending，
 *  不用干等 10s 超时。 */
struct AlarmResetResultPayload {
    uint16_t alarm_code;  /**< 关联正在复位的报警 */
    uint8_t  result;      /**< kAlarmResetResultFailed */
    uint8_t  reserved;
};

} // namespace middleware

#endif /* UPC_CONTRACT_HPP */

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
#define UPC_MOD_IPC 1
#endif
#ifndef UPC_MOD_MEM_POOL
#define UPC_MOD_MEM_POOL 1
#endif
#ifndef UPC_MOD_BUFFER_POOL
#define UPC_MOD_BUFFER_POOL 1
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

#endif /* UPC_DEBUG */

// ============================================================================
// §3 进程注册表（PID / 日志路径；daemon 实现见 process/process_ctl.hpp）
// ============================================================================

struct ProcessConfig {
    const char* pid_file;
    /** 日志路径前缀，实际文件为 {log_base}1.log ~ {log_base}3.log（5MB 轮转） */
    const char* log_base;
    const char* name;
};

/** 每进程独立 PID 与三文件循环日志（守护进程 start 时 stderr/stdout 重定向到 log_baseN.log） */
inline ProcessConfig process_config_ui() {
    return ProcessConfig{
        "/var/run/rongsu_ui.pid",
        "/var/log/rongsu_ui",
        "rongsu_ui",
    };
}

inline ProcessConfig process_config_glavo() {
    return ProcessConfig{
        "/var/run/rongsu_glavo.pid",
        "/var/log/rongsu_glavo",
        "rongsu_glavo",
    };
}

inline ProcessConfig process_config_camera() {
    return ProcessConfig{
        "/var/run/rongsu_camera.pid",
        "/var/log/rongsu_camera",
        "rongsu_camera",
    };
}

inline ProcessConfig process_config_broker() {
    return ProcessConfig{
        "/var/run/rongsu_broker.pid",
        "/var/log/rongsu_broker",
        "rongsu_broker",
    };
}

inline ProcessConfig process_config_xfr() {
    return ProcessConfig{
        "/var/run/rongsu_xfr.pid",
        "/var/log/rongsu_xfr",
        "rongsu_xfr",
    };
}

inline ProcessConfig process_config_visual() {
    return ProcessConfig{
        "/var/run/rongsu_visual.pid",
        "/var/log/rongsu_visual",
        "rongsu_visual",
    };
}

inline ProcessConfig process_config_mqtt() {
    return ProcessConfig{
        "/var/run/rongsu_mqtt.pid",
        "/var/log/rongsu_mqtt",
        "rongsu_mqtt",
    };
}

inline ProcessConfig process_config_media() {
    return ProcessConfig{
        "/var/run/rongsu_media.pid",
        "/var/log/rongsu_media",
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
    APP_Purify_Progress,      /**< [GLAVO_Purify_Progress_Ntf] Value */
    APP_Purify_Rsp,           /**< [GLAVO_Purify_Done] Empty */
    UI_Clean_Start_Req,       /**< Empty：开启清粉（开门前 PM2.5≥50 时触发） */
    APP_Clean_Start_Rsp,      /**< Empty：清粉完成（后端风机循环至 PM2.5 合格后回） */

    // ---- UI ↔ GLAVO：打印流程 ----
    UI_Print_Prepare_Req = 0x1040, /**< PrintPrepareReqPayload：轻量预处理 + USB 复制 */
    APP_Print_Prepare_Rsp,    /**< PrintPrepareRspPayload */
    UI_Parser_Exit_Ntf,       /**< Empty：取消解析（D1 X / 二次确认 Cancel） */
    UI_Print_Start_Req,       /**< PrintStartReqPayload：正式解析 + 下发（含基板厚度） */
    APP_Print_Progress_Ntf,   /**< [GLAVO_Print_Progress_Ntf] PrintProgressPayload */
    UI_Print_Terminate_Req,
    APP_Print_Terminate_Done, /**< [GLAVO_Print_Terminate_Done] Empty */
    UI_Print_Pause_Req,       /**< Empty：暂停打印（McuLink 停止 TCP 运动下发） */
    UI_Print_Resume_Req,      /**< Empty：恢复打印 */
    UI_Print_Powder_Low_Ignore_Req, /**< Empty：本次打印忽略缺粉自动暂停与缺粉通知 */
    APP_Print_Pause_Rsp,      /**< [GLAVO_Print_Pause_Rsp] PrintPauseStatePayload：暂停/恢复响应（Pause/Resume Req 共用） */
    APP_Print_Powder_Low_Ntf, /**< Empty：粉量传感器 bit0/bit1 同时为 0，触发一次缺粉通知 */

    // ---- UI ↔ GLAVO：耗材/滤芯 ----
    UI_Supply_Status_Req = 0x1060, /**< Empty */
    APP_Supply_Status_Rsp,    /**< [GLAVO_Supply_Status_Rsp] SupplyStatusPayload */

    // ---- UI ↔ CAMERA：视频流（控制面 socket；像素走 SHM） ----
    UI_Camera_Stream_Start_Req = 0x1070, /**< Empty 或 CameraStreamStartReqPayload */
    APP_Camera_Stream_Start_Rsp,         /**< CameraStreamInfoPayload */
    UI_Camera_Stream_Stop_Req,           /**< Empty */
    APP_Camera_Stream_Stop_Done,         /**< Empty */
    APP_Camera_Frame_Ntf,                /**< CameraFrameNtfPayload（可选唤醒 UI） */
    UI_Camera_Snapshot_Req,              /**< Empty：请求保存当前最新 NV12 帧到 /home/media/photo */
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
    UI_Mcu_Ota_Start_Req = 0x1090, /**< Empty：启动 Glavo(以太网) OTA，扫描 /home/image/glavo */
    UI_Mcu_Ota_Cancel_Ntf,         /**< Empty：中止 Glavo OTA 会话 */
    APP_Mcu_Ota_Status_Ntf,        /**< McuOtaStatusPayload（Glavo OTA 状态） */
    UI_Serial_Mcu_Ota_Start_Req,   /**< Empty：启动 MCU(串口) OTA，扫描 /home/image/mcu */
    UI_Serial_Mcu_Ota_Cancel_Ntf,  /**< Empty：中止 MCU 串口 OTA 会话 */
    APP_Serial_Mcu_Ota_Status_Ntf, /**< McuOtaStatusPayload（MCU 串口 OTA 状态） */

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
    UI_Debug_CleanTest_Start_Ntf,   /**< Empty：清粉测试开启） */
    UI_Debug_CleanTest_Close_Ntf,   /**< Empty：清粉测试关闭/中止 */
    UI_Debug_Fan_Duty_Req,          /**< DebugFanPayload：仅设风扇占空比（不开关） */
    UI_Debug_PrintParam_Set_Req,    /**< DebugPrintParamPayload：打印层参数 */
    UI_Debug_Laser_Power_Set_Req,   /**< DebugLaserPowerPayload：激光手动功率 */
    UI_Debug_Laser_Power_Reset_Req, /**< Empty：取消手动功率，恢复外控 */
    UI_Debug_Laser_AlarmReset_Req,  /**< Empty：激光器报警复位（固定 1301） */
    UI_Debug_Print_H264_Record_Set_Req, /**< DebugOnOffPayload：打印期连续 H264 录像开关（仅本次运行） */
    UI_Debug_Print_H264_Record_Get_Req, /**< Empty：查询当前打印期连续 H264 录像开关状态 */
    UI_Debug_Clean_Record_Set_Req,      /**< DebugOnOffPayload：清粉录像开关（仅本次运行） */
    UI_Debug_Clean_Record_Get_Req,      /**< Empty：查询当前清粉录像开关状态 */
    UI_Debug_Env_Sim_Set_Req,           /**< DebugOnOffPayload：环境上报模拟开关（仅本次运行，默认关） */
    UI_Debug_Env_Sim_Get_Req,           /**< Empty：查询当前环境上报模拟开关状态 */
    UI_Debug_Env_Sim_Powder_Set_Req,    /**< DebugOnOffPayload：环境模拟粉量缺粉开关（延迟 5s 生效） */
    UI_Debug_Env_Sim_Powder_Get_Req,    /**< Empty：查询当前环境模拟粉量缺粉开关状态 */

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
    GLAVO_Debug_Print_H264_Record_Ntf, /**< DebugOnOffPayload：打印期连续 H264 录像开关状态 */
    GLAVO_Debug_Clean_Record_Ntf,      /**< DebugOnOffPayload：清粉录像开关状态 */
    GLAVO_Debug_Env_Sim_Ntf,           /**< DebugOnOffPayload：环境上报模拟开关状态 */
    GLAVO_Debug_Env_Sim_Powder_Ntf,    /**< DebugOnOffPayload：环境模拟粉量缺粉开关状态 */

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
    APP_Clean_Test_Done_Ntf,     /**< CleanTestDonePayload：TLV 0x22 结果 + 调试保留标记 */
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
    APP_Visual_Camera_Stream_Stop_Done, /**< Empty */
    APP_Visual_Rect_Ntf,                /**< VisualRectNtfPayload */
    // ---- MEDIA ↔ CAMERA：媒体进程并行取流（拍照/录像） ----
    MEDIA_Camera_Stream_Start_Req = 0x1450,  /**< CameraStreamStartReqPayload（媒体侧取流） */
    APP_Media_Camera_Stream_Start_Rsp,       /**< CameraStreamInfoPayload */
    MEDIA_Camera_Stream_Stop_Req,            /**< Empty */
    APP_Media_Camera_Stream_Stop_Done,       /**< Empty */

    // ---- MQTT ↔ MEDIA：云端 RTSP 推流（live；recording 一期可拒） ----
    MQTT_Media_Rtsp_Start_Req,              /**< RtspPushStartReqPayload */
    APP_Mqtt_Rtsp_Start_Rsp,                /**< RtspPushRspPayload */
    MQTT_Media_Rtsp_Stop_Req,               /**< RtspPushStopReqPayload */
    APP_Mqtt_Rtsp_Stop_Rsp,                 /**< RtspPushRspPayload */

    // ---- UI ↔ GLAVO：MCU 固件版本查询（串口 PORT_GENERAL_TASK 0xA2 / CMD_PROJECT_INFO 0x10） ----
    UI_Mcu_Version_Req = 0x1500, /**< Empty：进入 Settings/About 页时查询一次 */
    GLAVO_Mcu_Version_Ntf,       /**< McuVersionPayload：MCU 固件版本 + 板级配置 */

};

enum class MsgPriority : uint8_t {
    Low       = 0,
    Normal    = 1,
    High      = 2,
    Realtime  = 3
};

// Payload 契约（POD，可 memcpy；跨进程由 ipc_wire 序列化）
// 命名：\<动作\>Payload / \<动作\>ReqPayload / \<动作\>RspPayload

enum class UserHomeStatus : uint32_t {
    Idle     = 0,
    Ready    = 1,
    Printing = 2,
    Error    = 3,
};

/** APP_Update_Ntf 载荷：首页环境/状态快照（Inline）
 *  尾部三字段为待机首页新增指标；后端未采集到时填 <0（或 NaN）表示无效，
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
    float    pm10_indoor;       /**< 室内 PM10 (μg/m³)；无效填 <0（HOME 卡切换指标用） */
    float    humidity_percent;  /**< 舱内湿度 %；无效填 <0 */
    float    pressure_kpa;      /**< 舱内气压 kPa；无效填 <0 */
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

/** UI_Print_Start_Req：来源 + 索引 + 基板厚度（um） */
struct PrintStartReqPayload {
    uint32_t        file_index;
    PrintFileSource file_source;
    uint8_t         reserved;
    uint16_t        plate_thickness_um; /**< 例如 5mm=5000，10mm=10000 */
    uint8_t         reserved2[2];
};

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
    uint16_t cmd_cache_rate;   /**< MCU 指令缓存占用率（TCP 心跳，0=排空） */
    uint16_t data_cache_rate;  /**< MCU 数据缓存占用率（TCP 心跳，0=排空） */
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

struct VisualRectPoint {
    int32_t x;
    int32_t y;
};

/** APP_Visual_Rect_Ntf：视觉识别矩形四点（Inline） */
struct VisualRectNtfPayload {
    uint8_t valid;          /**< 1=检测到矩形；0=当前无矩形 */
    uint8_t reserved[3];
    uint32_t frame_width;   /**< 源图宽（像素） */
    uint32_t frame_height;  /**< 源图高（像素） */
    uint32_t frame_seq;     /**< 对应相机帧序号（若未知可填 0） */
    VisualRectPoint pts[4]; /**< 顺时针四点坐标，单位=源图像素 */
};

/** GLAVO→CAMERA 打印录像会话（Start/Finish/Abort 共用） */
struct PrintVideoSessionPayload {
    char     session_id[32];
    char     job_name[64];   /**< 用于输出 /home/media/video/{job}/ 目录名 */
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
 *   - `has_entry=1` → 以 `frames_dir` 为主键在 `/home/media/video/config.json` 定位该条，
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

/** UI_Clean_Test_Start_Req：打印结果页 Purify 按钮触发清粉测试 + 录像 */
struct CleanTestStartReqPayload {
    char job_name[64];   /**< 对应 /home/media/video/{job_name}/ */
    uint32_t width;      /**< 0=跟随 Camera 当前预览宽 */
    uint32_t height;     /**< 0=跟随 Camera 当前预览高 */
    uint32_t fps;        /**< 0=跟随 Camera 当前预览 fps */
};

/** APP_Clean_Test_Start_Rsp：清粉测试请求是否被 Glavo 受理 */
struct CleanTestStartRspPayload {
    int8_t accepted;     /**< 1=已受理 0=拒绝（如已有进行中的清粉测试） */
    uint8_t reserved[3];
};

/** APP_Clean_Test_Done_Ntf：串口 TLV 0x22 完成结果（0=成功，非0=失败）。
 * 有清粉录像时由 Media 在清粉抽帧停止后发送；最终 MP4 在结果页退出后统一合成。 */
struct CleanTestDonePayload {
    uint8_t result_code;   /**< 0=成功；非0=失败 */
    uint8_t raw_retained;  /**< 兼容字段；PNG 路径下恒为 0 */
    uint8_t has_mp4;       /**< 1=期望存在合并后的 timelapse.mp4 */
    uint8_t reserved;
    char    mp4_rel_path[96]; /**< 相对 /home/media/video 的路径，如 job/timelapse.mp4 */
};

/** UI→MEDIA：结果页退出后触发统一 MP4 合成（按 frames_dir 定位会话目录） */
struct PrintVideoFinalizeReqPayload {
    char frames_dir[96]; /**< 对应 /home/media/video/{frames_dir}/ */
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
    uint8_t result_code;  /**< 0=成功；非0=失败；0xfe=用户关闭 */
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

// 调试页（手动控制/标定）契约 —— 命名见 common/COMMON.md；枚举跨进程时按 uint8_t 传输

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

#endif /* UPC_CONTRACT_HPP */

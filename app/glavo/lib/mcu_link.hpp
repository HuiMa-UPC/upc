#ifndef MCU_LINK_HPP
#define MCU_LINK_HPP

#include "slice_ring_buffer.hpp"
#include "parser_config.hpp"
#include "device_config.hpp"
#include "protocol.hpp"
#include "mcu_ota_session.hpp"
#include "mcu_serial_ota_session.hpp"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace upc::glavo {

/**
 * @brief MCU 链路（TCP + 串口）打印任务事件
 *
 * 用于向上层（GlavoService / UI）报告打印完成、终止或错误。
 */
enum class McuLinkEvent : uint8_t {
    PrintCompleted = 0, /**< 打印自然完成（RingBuffer 已空、解析器已结束且 MCU 缓存已排空） */
    TerminateDone,      /**< 用户手动终止或异常中断后的清理已完成 */
    PrintError,         /**< 打印过程中发生错误（如 TCP 断线）导致的终止 */
    PowderLow,          /**< 打印中检测到缺粉（0x9B value[10] bit0/bit1 == 00） */
    OxygenHigh,         /**< V4：打印中 SOC 氧 ≥ 阈值，已 TCP 暂停；上层可同步 UI 暂停态 */
    OxygenRecovered,    /**< V4：打印中 SOC 氧回落到恢复线，已解除氧暂停 */
    OuterDoorClosed,    /**< 外门开→关边沿：上层可跑关内门套件（降 Z / 关内门 / 回 Z） */
    /** 起印氧门：CONDITION_OK 后已开洗气、正在等 SOC 氧达标（可重复；上层去重） */
    OxygenGateWaiting,
    /** 起印氧门通过，即将发 PRINT_START（每作业握手段至多一次） */
    OxygenGatePassed,
};

/** @brief 链路事件回调类型：接收到 McuLinkEvent 时通知上层 */
using McuLinkEventCb = std::function<void(McuLinkEvent)>;
/** @brief 进度回调类型：每成功下发一批运动包后触发，用于更新进度条等 UI */
using McuLinkProgressCb = std::function<void()>;
/** @brief 清粉测试完成回调：参数为清粉完成上报 result_code（A2/42/0x03）。 */
using McuLinkCleanTestDoneCb = std::function<void(uint8_t)>;
using McuLinkTransportResultCb = std::function<void(uint8_t)>;
/** @brief 首层视觉检测回调：true=通过，false=不通过（继续铺粉）。 */
using McuLinkFirstLayerCheckCb = std::function<bool(uint16_t)>;

/** @brief 环境传感器缓存（串口 0x9B + SOC 本地采集合并，供 UI/打印进度读取） */
struct EnvSnapshotCache {
    float pm25_indoor     = 0.f;   /**< 舱外/室内常显 PM2.5（V4=SOC UART 舱外） */
    float pm25_cabin      = 0.f;   /**< 舱内 PM2.5（V4=SOC UART 舱内，阀关时可能无效） */
    float pm1_indoor      = -1.f;  /**< 舱外 PM1.0；无源时 <0 */
    float pm10_indoor     = -1.f;  /**< 舱外 PM10；无源时 <0 */
    float pm1_cabin       = -1.f;  /**< 舱内 PM1.0（V4 SOC）；无源时 <0 */
    float pm10_cabin      = -1.f;  /**< 舱内 PM10（V4 SOC）；无源时 <0 */
    float temperature_c   = 0.f;
    float oxygen_percent  = 0.f;
    float filter_oxygen_percent = -1.f; /**< 滤芯氧 %（V4 MCU 1ST）；无源时 <0 */
    float humidity_percent = 0.f;
    float pressure_kpa    = -1.f;   /**< 舱内气压 kPa（旧=MCU；V4=SOC CPM121） */
    float baseplate_temp_c = -1.f;  /**< 基板温度 ℃；无源时 <0 */
    float filter_pressure_kpa = -1.f; /**< 滤芯气压 SC0（MCU 1ST） */
    uint8_t powder_bits   = 0u;     /**< 旧：bit0/bit1；V4：红外低 2 位兼容 */
    uint8_t powder_ir_bits = 0u;    /**< V4：A1..OUTR 八路红外 */
    bool  outer_door_open = false;  /**< false=门关 true=门开 */
    bool  door_state_valid = false; /**< 是否已收到至少一帧可用门状态 */
    bool  sensor_v4       = false;  /**< 当前按 V4 协议路径填缓存 */
    bool  valid           = false;
};

/**
 * @brief MCU 双通道链路管理（TCP 运动 + 串口握手/传感器，单例）
 *
 * 职责：
 * - TCP：与振镜控制板长连接、心跳、运动包批量下发、MCU 速率反馈；
 * - 串口：打印握手、换层、终止/完成、环境传感器上报；
 * - 从 RingBuffer 批量取运动包，按协议组帧后下发；
 * - 处理换层标记（Layer Mark），通过串口与 MCU 进行层间握手；
 * - 支持用户主动终止（terminate）与自然完成两种结束路径。
 *
 * 线程模型：所有网络/串口 I/O 与协议处理集中在独立的 link_thread_fn 线程中
 *（轮询 + link_thread_sleep_ms 延时）；对外接口仅做状态设置与原子标志操作。
 */
class McuLink {
public:
    /** @brief 获取 McuLink 单例实例 */
    static McuLink& instance();

    /** @brief 绑定 RingBuffer，作为运动包的数据来源 */
    void bind_ring_buffer(upc::parser::RingBuffer* rb);

    /** @brief 设置链路事件回调（如打印完成、终止完成） */
    void set_job_event_callback(McuLinkEventCb cb);

    /**
     * @brief 设置进度回调
     * @note 每成功下发一批运动包后，在链路线程中调用。
     */
    void set_progress_callback(McuLinkProgressCb cb);
    void set_clean_test_done_callback(McuLinkCleanTestDoneCb cb);
    void set_transport_result_callback(McuLinkTransportResultCb cb);
    void set_first_layer_check_callback(McuLinkFirstLayerCheckCb cb);

    /** @brief 运行时设置串口 ACK 可靠发送开关（默认值来自编译宏）。 */
    void set_serial_ack_enabled(bool enabled);
    [[nodiscard]] bool serial_ack_enabled() const noexcept;

    /**
     * @brief 建立 TCP 连接（非阻塞 connect + poll 超时，地址见 GLAVO_TCP_DEFAULT_IP/PORT）
     * @return 0 成功；-1 socket 失败；-2 IP 非法；-3 connect 立即失败；
     *         -4 poll 超时或出错；-5 连接最终失败（SO_ERROR）
     */
    int connect_tcp();

    /**
     * @brief 打开串口设备（单次尝试）
     * @param device 串口设备路径，如 "/dev/ttyUSB0"
     * @param baud   波特率，当前支持 9600 / 115200（默认 115200）
     * @return 0 表示成功，负值表示失败
     */
    int open_serial(const char* device, int baud);

    /** @brief 启动链路线程（建议在 connect_tcp / open_serial 成功后调用） */
    void start_link_thread();

    /** @brief 停止链路线程，关闭 TCP 与串口 fd，重置所有状态 */
    void stop();

    /** @brief TCP 是否处于已连接状态（fd >= 0） */
    bool is_tcp_connected() const;
    /** @brief 当前是否处于“正在下发运动包”阶段 */
    bool is_sending() const;

    /** @brief 是否已请求终止、待链路线程执行清理（尚未发 TerminateDone） */
    [[nodiscard]] bool is_terminate_pending() const noexcept;

    /**
     * @brief 用户确认开始打印后调用
     *
     * 若 RingBuffer 中可用包数不足，则设置 start_pending_ 标志，
     * 待 RingBuffer 累积到阈值后由链路线程自动启动。
     * terminate/cleanup 进行中会拒绝起印（避免与清理竞态）。
     * @param skip_layer_handshake true=跳过串口首层/换层握手（测试图无 M3000）
     */
    void begin_sending(bool skip_layer_handshake = false);

    /**
     * @brief 标记当前运动作业为调试测试图模式
     *
     * 自然完成时跳过末层 Snap 与串口 PRINT_FINISHED（仍发 TCP Stop + PrintCompleted）。
     */
    void set_test_image_motion(bool on) noexcept;
    [[nodiscard]] bool is_test_image_motion() const noexcept;

    /** @brief 请求异步终止打印（用户手动停止或 TCP 断线触发） */
    void request_terminate();

    /**
     * @brief 用户暂停/恢复：仅 gate TCP 运动包批量下发（`link_send_one_motion_batch`）
     * @note 不影响 is_sending_、解析线程、换层串口与 TCP 心跳
     */
    void set_user_dispatch_paused(bool paused);
    [[nodiscard]] bool is_user_dispatch_paused() const noexcept;
    /** @brief V4 SOC 氧过高导致的运动保持（与用户暂停独立） */
    [[nodiscard]] bool is_o2_hold() const noexcept;
    /**
     * @brief 下发 TCP CMD=0x0A 通用紧急指令（Stop/Pause）
     * @param stop  true=急停/终止（最高优先级）
     * @param pause true=暂停，false=继续/不暂停
     * @return true=组帧并发送成功，false=TCP 未就绪或发送失败
     */
    bool send_tcp_emergency_ctrl(bool stop, bool pause);
    /**
     * @brief 下发 TCP CMD=0x0A 扩展紧急指令（Stop/Pause/RedLaser）
     * @param stop_code DATA[1]：1=急停，0=不修改
     * @param pause_code DATA[2]：2=暂停，1=继续，0=不修改
     * @param red_code DATA[3]：3=红光常亮，2=激光，1=红光，0=不修改
     */
    bool send_tcp_emergency_ctrl_ex(uint8_t stop_code, uint8_t pause_code, uint8_t red_code);
    /**
     * @brief 调试专用：直接下发一个已编码的 11 字节运动子包
     * @note 仅允许在无打印/测试图运动流、无终止清理时调用。
     *       光路自检 / 点位测试走这条；测高与高度校准不要用。
     */
    bool send_debug_motion_subpacket(const uint8_t subpacket[11]);
    /**
     * @brief 单点振镜绝对坐标移动（手动控制 / 测高扫描用）
     *
     * 内部走与切片下发完全一致的编码口径：
     *   cal_apply_xy(校准畸变) → × scale_gain → coordinate_to_offset_bytes
     *   → 构造 11B 运动包 → build_motion_frame → tcp_send_raw
     *
     * 打印进行中（is_sending_）会拒绝调用，避免与批量运动包冲突。
     *
     * @param x_mm, y_mm     目标坐标(mm，振镜坐标系，0,0 为中心；越界自动截断)
     * @param speed_mm_min    扫描速度，<=0 时用默认 1000mm/min
     * @param laser_on        瞄准移动应传 1（LD=01 00）。0=空跳会途中关红光。
     *                        本函数只给手动/测高用；打印空跳走 Parser，不走这里。
     * @return true=组帧并发送成功；false=TCP 未就绪 / 打印中互斥 / 发送失败
     */
    bool send_galvo_abs_pos(float x_mm, float y_mm,
                            float speed_mm_min, int laser_on);
    /** @brief 最近一次 MCU 运动位置上报的 Z 实际位置（mm）。 */
    [[nodiscard]] float get_z_pos_mm() const noexcept;
    /**
     * @brief 标记“打印任务流程”是否激活（用于门控高频心跳回包日志）
     * @note 业务约定：从进入打印到用户滑动开门解锁成功，均视为同一流程。
     */
    void set_print_task_flow_active(bool active) noexcept;
    [[nodiscard]] bool is_print_task_flow_active() const noexcept;

    /** @brief 运动已发完、正在等 MCU 双缓冲排空（自然完成尾段） */
    [[nodiscard]] bool is_natural_complete_pending() const noexcept;
    /** @brief 换层/首层串口握手等待中（TCP 运动下发暂停） */
    [[nodiscard]] bool is_waiting_layer() const noexcept;
    /** @brief 本作业曾观测到 MCU 缓存非空（防初值 0,0 误判已排空） */
    [[nodiscard]] bool mcu_rates_have_been_nonempty() const noexcept;

    /** @brief 设置 MCU OTA 状态回调（测试） */
    void set_ota_status_callback(McuOtaStatusCb cb);
    void set_serial_ota_status_callback(McuOtaStatusCb cb);

    /** @brief 开始 MCU OTA（扫描 image_dir 下最新 .bin） */
    bool start_mcu_ota(const char* image_dir = nullptr);
    bool start_serial_mcu_ota(const char* image_dir = nullptr);

    /** @brief 测试用：中止 SOC 侧 OTA 会话 */
    void cancel_mcu_ota();
    void cancel_serial_mcu_ota();

    [[nodiscard]] bool ota_active() const;
    [[nodiscard]] bool serial_ota_active() const;

    /**
     * @brief 将串口发送帧入队，由 link_thread 统一发送（线程安全）
     * @param reliable true 时经 serial_send_aa_reliable 走带 ACK 的可靠通道
     *                 （需 serial_ack_enabled()=true 才真正重传，否则等同裸发）；
     *                 false 时裸发不等 ACK。
     * @note 队列满时丢弃最旧一条
     */
    void enqueue_serial_tx(const uint8_t* data, size_t len, bool reliable = false);
    /**
     * @brief 本轮串口队列 drain 之后再发 TCP 红光（Red=1 或 Red=3）。
     *        给「先入队 0x12、再开瞄准红光」用。切激光会作废这次补发。
     */
    void request_reassert_red_after_serial_tx(uint8_t red_code);
    /** @brief 作废尚未执行的红光补发（切激光 / 普通红光时调用）。 */
    void cancel_pending_red_reassert();

    /** @brief 读取最新环境传感器快照（线程安全） */
    [[nodiscard]] EnvSnapshotCache env_snapshot() const;

    /** @brief 当前/下层铺粉高度（微米）；未开打时为默认层高 */
    [[nodiscard]] uint16_t active_layer_height_um() const;

    /**
     * @brief MCU 固件是否已报到且 major≥4（传感器走 V4 布局 + SOC 本地采集 / 开阀采样）。
     * unknown / major&lt;4 → false：旧板无 fa_valve，不开阀。
     */
    [[nodiscard]] bool mcu_sensor_protocol_v4() const noexcept;

    /** @brief 读 MCU 版本缓存（About / 门控逻辑） */
    [[nodiscard]] middleware::McuVersionPayload mcu_version_cached() const;

    /**
     * @brief 调试：环境上报模拟开关（仅本次运行，默认关）
     * 开启后用变化的测试数据驱动 APP_Update_Ntf（供 MQTT 联调），并跳过串口真实传感器写缓存。
     */
    void set_env_sim_enabled(bool on);
    [[nodiscard]] bool env_sim_enabled() const noexcept;
    /**
     * @brief 调试：跳过 V4 SOC 氧打印门（仅本次运行，默认关）
     * @note 开：起印不发洗气开、不等氧、打印中不因氧高 pause/1120；关：恢复开阀洗气+氧门。
     */
    void set_skip_o2_print_gate(bool on);
    [[nodiscard]] bool skip_o2_print_gate() const noexcept;
    /**
     * @brief 是否启用 SOC 氧门+打印前洗气（非测试图、非 Env Sim、未 Skip O2）
     * @note 关时：起印不发洗气开、不等氧；开时：CONDITION_OK 后 0x86 开阀再等氧。
     */
    [[nodiscard]] bool soc_o2_print_gate_enabled() const noexcept;
    /** @brief 调试：环境模拟粉量缺粉目标开关（延迟 5s 生效） */
    void set_env_sim_powder_low(bool on);
    [[nodiscard]] bool env_sim_powder_low() const noexcept;

    /**
     * @brief 发送“打印前开门请求”并等待 MCU 结果（TLV_REQ_OPEN_DOOR / TLV_REQ_DOOR_OPEN_RESULT）
     * @param timeout_ms 等待上限（毫秒）
     * @param opened_out 可选输出：true=门已开（door_state=0），false=拒绝/失败
     * @return true=收到结果；false=超时或发送失败
     */
    bool request_open_door_and_wait(uint32_t timeout_ms, bool* opened_out);

    /**
     * @brief 仅发送一次开门指令（TLV_REQ_OPEN_DOOR），不等待 0x11 结果回包
     * @return true=已成功入串口发送队列；false=串口未就绪或组帧失败
     */
    bool request_open_door_once();

    /**
     * @brief 仅发送一次自动转运请求（TLV_REQ_TRANSPORT 0x02），不等待结果
     * @return true=已入串口发送队列；false=串口未就绪或组帧失败
     */
    bool request_transport_once();

    /**
     * @brief 设置 TCP_NODELAY，禁用 Nagle 算法以降低发包延迟
     * @param sockfd 已连接的 socket 文件描述符
     */
    static bool set_tcp_nodelay(int sockfd);

    /**
     * @brief 串口帧解析后的事件分发入口（供链路线程 RX 使用）
     * @param evt 由 serial_parse_frame 解析出的串口事件
     */
    void notify_serial_event(serial_event_t evt);

    void handle_serial_rx_frame(const uint8_t* frame, size_t len);

    void on_sensor_report(const serial_sensor_report_t& report);
    void on_sensor_ext_report(const serial_sensor_ext_t& ext);
    void merge_soc_env_into_cache_locked();
    void refresh_soc_v4_mode();
    void publish_debug_soc_led_ntf();
    void on_motion_pos_report(const serial_motion_pos_t& pos);
    void publish_env_update_ntf();
    /** @brief 仅向 UI 推送状态快照（Ready/Idle + WiFi），不清空环境值也不推 MQTT。 */
    void publish_env_status_ntf_ui_only();
    /** @brief 强制推送环境无效哨兵（各 float <0），供 UI 显示 "--"；不要求 env_cache_.valid */
    void publish_env_invalid_ntf();
    void publish_debug_sensor_ntf(const serial_sensor_report_t& report);
    /** @brief 调试页传感器条推无效哨兵（<0 → UI "--"） */
    void publish_debug_sensor_invalid_ntf();
    /**
     * @brief V4：用 SOC 快照 + 已缓存的 MCU 字段推调试顶栏（不依赖本周期 1ST）
     * @note 未收到过 MCU 1ST 时 O2/基板/粉量发无效哨兵；PM/温湿度/气压走 SOC。
     */
    void publish_debug_sensor_from_soc_and_cache();
    void publish_debug_axis_ntf(const serial_motion_pos_t& pos);
    void on_mcu_version_report(const serial_project_info_t& info);
    void publish_mcu_version_ntf();
    void drain_serial_tx_queue();
    /** @brief MQTT 环境 IPC 发送节流；force=true 用于关闭模拟时立即推清空帧 */
    bool should_publish_env_to_mqtt(bool force = false) noexcept;
    /** @brief 调试环境模拟：周期写假数据并 publish_env_update_ntf */
    void poll_env_sim_tick();
    /**
     * @brief V4：SOC 本地采集独立推 UI（APP_Update_Ntf + 调试顶栏）
     * @note 旧固件不跑；Env Sim 开启时跳过。与 MCU 1ST 解耦，避免无 1ST 时顶栏/首页一直 "--"。
     */
    void poll_soc_env_tick();
    /** @brief 关闭模拟后清缓存并推送无效哨兵，等待真实 0x9B 再恢复 */
    void clear_env_after_sim_disable();

    /** TCP 常态应答（运动/OTA），供分帧回调使用 */
    void on_tcp_status_response(const tcp_response_t& resp);
    /** TCP 初始化上报（ID=0x0F）：用于版本识别与发送参数档位切换 */
    void on_tcp_init_report(bool has_version, uint32_t fw_version_le,
                            uint8_t firmware_id, uint8_t ota_state,
                            uint8_t selfcheck_boot_count);

    /**
     * @brief 将缓存的振镜控制板固件版本推给 UI（GLAVO_Galvo_Version_Ntf）
     * @note TCP 0x0F 扩展包到达时会主动推一次；About 进页 Req 再调一次读缓存。
     *       尚未收到带版本的首包时 valid=0。
     */
    void publish_galvo_version_ntf();
    /**
     * @brief Env Sim 打开时补发模拟版本号（仅 unknown 时生效，不覆盖真实版本）
     */
    void publish_simulated_versions_if_unknown();

    /**
     * @brief 向 MCU 请求一轮报警快照（0xA3/0x81 TLV 0x05）
     * @note MCU **不会主动推**快照。串口打开成功后自动调一次，补齐断连期间
     *       错过的状态变化；UI 侧另有启动/进页/轮询等触发点（那边带节流）。
     */
    void request_alarm_snapshot();
    /**
     * @brief 调试页 Alarm Inject：把一帧单报只发给 MQTT（不上 UI、不改快照/灯效）
     * @note UI 自己走 `ui_alarm_debug_inject` 带 `[SIM]`；这里只为云端 `alarm.single`。
     */
    void publish_debug_alarm_inject(const middleware::AlarmReportPayload& p);
    /**
     * @brief 收齐 MCU 快照一轮后转发 UI；若 SOC 1120 活跃且缺 slot3 则补 bit20
     * @note UI 要求同轮所有分片 nonzero_slot_count 一致，故须在 Glavo 攒齐再发。
     */
    void ingest_alarm_snapshot_slot(const serial_alarm_snapshot_t& snap);

private:
    struct SerialReliableItem {
        uint8_t data[64]{};
        size_t  len = 0;
        bool    abort_print_on_fail = false;
    };

    /** MCU 报警快照攒帧（与 UI 同语义；用于并入 SOC 1120 后再转发） */
    struct AlarmSnapAssembly {
        bool busy = false;
        bool meta_known = false;
        uint8_t count = 0;
        uint8_t severity = 0;
        uint16_t got_mask = 0;
        uint8_t got_count = 0;
        serial_alarm_snapshot_t slots[SERIAL_ALARM_SNAPSHOT_SLOTS]{};
        std::chrono::steady_clock::time_point started{};
    };

    void reset_alarm_snap_assembly();
    void flush_alarm_snap_assembly();
    static constexpr int kAlarmSnapAssembleMs = 1500;

    McuLink();
    ~McuLink();
    McuLink(const McuLink&) = delete;
    McuLink& operator=(const McuLink&) = delete;

    /** @brief 链路线程主函数：负责心跳、RX 排空、运动包下发、状态机推进 */
    void link_thread_fn();

    /**
     * @brief TCP 原始发送（非阻塞完整写：短写续传，EAGAIN 分片等 POLLOUT）
     * @return 0 整帧写完；-1 未写出、可整批重试；-2 半写/hangup，禁止整批重发
     */
    int tcp_send_raw(const uint8_t* data, size_t len);
    /** @brief TCP 非阻塞接收，返回读取到的字节数（0 表示暂无数据，-1 表示出错或断开） */
    int tcp_recv_nonblock(uint8_t* buf, size_t buf_size);
    /** @brief 排空 TCP 接收缓冲区，并做帧解析与处理 */
    void tcp_drain_rx();

    /** @brief 串口原始发送 */
    int serial_send_raw(const uint8_t* data, size_t len);
    /** @brief 串口非阻塞接收 */
    int serial_recv_nonblock(uint8_t* buf, size_t buf_size);
    /** @brief 排空串口接收缓冲区，并做帧解析与事件分发 */
    void serial_drain_rx();

    void on_serial_mcu_ack(const uint8_t* frame, size_t len);
    void send_soc_ack_for_bb(const uint8_t* frame, size_t len);
    void serial_send_aa_reliable(const uint8_t* data, size_t len,
                                 bool abort_print_on_fail);
    void link_process_serial_reliable_tx();
    void serial_clear_reliable();
    void serial_start_reliable_item(const SerialReliableItem& item);
    void serial_advance_reliable_queue();

    /** @brief 清空 tcp_stage_ 暂存队列 */
    void tcp_stage_clear();
    /** @brief 获取 tcp_stage_ 当前包数（线程安全） */
    size_t tcp_stage_count();
    /** @brief 查看队列头部包的类型，不移除（线程安全） */
    bool tcp_stage_peek_type(upc::parser::rb_pkt_type_t* ptype);
    /** @brief 从队列头部取出一个包（线程安全） */
    bool tcp_stage_pop_front(upc::parser::rb_packet_t* pkt);
    /** @brief 从 RingBuffer 批量预取包到 tcp_stage_，减少链路线程加锁粒度 */
    void refill_tcp_stage_from_ring();

    /**
     * @brief 重置运动/发送相关状态
     * @param clear_start_pending 是否同时清除 start_pending_ 标志
     */
    void reset_motion_state(bool clear_start_pending);
    /** @brief 内部真正开始发送：初始化层高、状态标志，并可选发起串口打印请求握手 */
    void begin_sending_internal(); /**< 使用 test_image_motion_ 决定是否跳过串口握手 */
    /** @brief 判断 RingBuffer 是否满足开始发送的阈值条件 */
    bool ring_meets_start_threshold() const;
    /** @brief 若存在挂起的 begin_sending 且阈值已满足，则正式启动 */
    void try_complete_pending_start();

    /** @brief 主链路逻辑：尝试从 tcp_stage_ 批量下发运动包 */
    void link_drain_motion_queue();
    /** @brief 尝试结束打印：检查 RingBuffer、解析器、MCU 缓存是否全部排空 */
    void link_try_finish_sending();
    /**
     * @brief 处理队列头部的换层标记（Layer Mark）
     * @return true 表示成功识别并进入换层等待状态
     */
    bool link_handle_layer_mark();
    /** @brief 若 MCU 缓存已排空，向串口发送“本层已完成”通知，启动下一层握手 */
    void link_try_layer_serial_dispatch();
    /** @brief 打印自然完成前补末层 Snap（文件末尾无 M3000 时） */
    void link_try_final_layer_snap();
    /** @brief 从 tcp_stage_ 取一批运动包，组帧后通过 TCP 下发 */
    int link_send_one_motion_batch();
    /** @brief 处理串口打印前的握手状态机（CONDITION_OK → 洗气/氧门 → PRINT_START → PRINT_READY） */
    void link_process_serial_handshake();
    /** @brief V4 打印中 SOC 氧门：超限暂停 / 回落恢复 + 1120 报警 */
    void link_process_o2_print_gate();
    /** @brief 读当前用于门控的氧浓度；false=无效（未就绪/读失败） */
    [[nodiscard]] bool read_print_o2_percent(float* oxygen_percent) const;
    /** @brief SOC 合成 1120 报警边沿（ACTIVE/NORMAL）并驱动灯效板 */
    void publish_soc_chamber_oxygen_alarm(bool active);

    /** @brief 向振镜发送终止指令（TCP CMD=0x0A，Stop=1，Pause=0） */
    void send_tcp_reset();
    void send_serial_layer_done(uint16_t next_layer_um);
    /** @brief 向 MCU 串口发送打印终止帧 */
    void send_serial_print_terminate();
    /** @brief 向 MCU 串口发送打印完成帧 */
    void send_serial_print_finished();
    /** @brief 洗气阀串口开关（Port 0xA1 Cmd 0x86）；target=1 */
    void send_serial_gas_wash(bool on);
    /** @brief 本作业 SOC 曾开过洗气则发关阀并清标志 */
    void close_print_gas_wash_if_opened();
    /** @brief 氧门开启时确保已发洗气开（幂等） */
    void ensure_print_gas_wash_on();
    /**
     * @brief 执行手动终止后的清理：发 reset、清队列、回调 TerminateDone/PrintError
     * @return true 本轮确实执行了清理并已上报事件；false 已有 cleanup 在进行，
     *         调用方**必须**保留 terminate_pending_ 以便下一轮重试
     * @note 返回 false 时不会发 reset、也不会 emit 任何事件；若调用方仍无条件清掉
     *       terminate_pending_，这次终止请求就被静默吃掉（振镜不停、UI 收不到回执）
     */
    [[nodiscard]] bool run_manual_terminate_cleanup();
    /** @brief 执行自然完成后的清理：发 finished、回调 PrintCompleted */
    void run_natural_complete_cleanup();
    void emit_event(McuLinkEvent ev);
    void poll_mcu_drain_heartbeat();
    void ota_tick();
    void serial_ota_tick();
    /** @brief OTA Success 后尝试 connect_tcp（含强制刷新已有 fd） */
    void link_try_ota_post_reconnect();

    /**
     * @brief 判断 MCU 的指令缓存与数据缓存是否均已排空
     * @param pending_packets 本次需确认排空的运动包数（换层取层内包数，完成取全作业包数）；
     *                        <= 0 表示尚未下发任何包，无需确认
     * @param since 进入排空等待状态的时刻；默认构造（epoch 0）表示尚未开始计时
     *
     * rate 必须先降到 MCU_DRAIN_MAX_RATE 以下；随后若本作业曾观测到缓存非空则直接放行，
     * 否则按 MCU_DRAIN_SMALL_BATCH_MS / MCU_DRAIN_FALLBACK_MS 兜底放行，
     * 避免超薄层在两帧心跳之间跑空导致的永久自锁。
     */
    bool mcu_rates_drained(int pending_packets,
                           std::chrono::steady_clock::time_point since) const;
    /** @brief 当前是否处于“应持续发运动包”的活跃流状态 */
    bool motion_stream_active() const;
    /** @brief 当前是否因 MCU 指令缓存达到阈值而暂停运动包下发 */
    bool motion_stream_backpressured() const;
    /** @brief 根据当前状态决定链路线程每次循环的休眠间隔（ms） */
    int link_thread_sleep_ms() const;
    /** @brief 获取当前生效的每轮 TCP 运动包发送上限 */
    int current_tcp_batch_size() const noexcept;
    /** @brief 获取当前生效的活跃下发轮询间隔（ms） */
    int current_tcp_send_interval_ms() const noexcept;
    /** @brief 获取当前生效的空闲心跳间隔（ms） */
    int current_tcp_heartbeat_interval_ms() const noexcept;
    /** @brief 获取当前生效的 MCU 指令缓存背压阈值（0-100） */
    uint32_t current_cmd_backpressure_pct() const noexcept;
    /** @brief 在打印空闲时应用挂起的 TCP 档位切换 */
    void try_apply_pending_tcp_profile();
    /** @brief 立即应用 TCP 档位参数（legacy / small-mem） */
    void apply_tcp_profile(bool small_mem, const char* reason,
                           uint32_t fw_version_le);
    /** @brief TCP 刚连上：只发振镜红光（Red=1），不跟串口 0x12。 */
    void force_switch_to_red_mode_on_tcp_connect();
    /** @brief 串口 MCU 真实版本上报后：关激光使能；下一轮 drain 后再补 TCP 红光。 */
    void apply_serial_laser_off_after_mcu_ready();
    /** @brief 0x12 出队后再补 TCP 红光，避免关使能把瞄准红光掐灭。 */
    void maybe_reassert_red_after_serial_laser_off();

    /* ---------- 核心依赖 ---------- */
    upc::parser::RingBuffer* rb_ = nullptr;          /**< 绑定的 RingBuffer，运动包来源 */
    McuLinkEventCb event_cb_;            /**< 链路事件回调 */
    McuLinkProgressCb progress_cb_;         /**< 进度更新回调 */
    McuLinkCleanTestDoneCb clean_test_done_cb_; /**< 清粉测试完成回调 */
    McuLinkTransportResultCb transport_result_cb_; /**< 自动转运结果回调 */
    McuLinkFirstLayerCheckCb first_layer_check_cb_; /**< 首层视觉检测回调 */

    /* ---------- 文件描述符 ---------- */
    int tcp_fd_    = -1; /**< TCP socket 文件描述符，-1 表示未连接 */
    int serial_fd_ = -1; /**< 串口文件描述符，-1 表示未打开 */

    /* ---------- 链路线程控制 ---------- */
    std::thread link_th_;             /**< 链路线程句柄 */
    std::atomic<bool> link_run_{false};  /**< 线程是否应继续主循环 */

    /* ---------- 发送状态机 ---------- */
    std::atomic<bool> is_sending_{false};              /**< 是否正在下发运动包 */
    std::atomic<bool> user_paused_{false};             /**< 用户暂停：停止 TCP 运动下发，解析与换层逻辑继续 */
    std::atomic<bool> o2_hold_{false};                 /**< V4 SOC 氧过高：停运动下发（与 user_paused_ 独立） */
    std::atomic<bool> print_task_flow_active_{false};  /**< 打印任务流程窗口（开始打印→开门解锁） */
    std::atomic<bool> test_image_motion_{false};       /**< 调试测试图：跳过串口握手/末层 Snap/PRINT_FINISHED */
    std::atomic<bool> start_pending_{false};           /**< begin_sending 已调用但 RingBuffer 未达阈值，等待中 */
    std::atomic<bool> terminate_pending_{false};       /**< 收到终止请求，待链路线程执行清理 */
    std::atomic<bool> natural_complete_pending_{false};/**< 运动包已发完，等待 MCU 缓存排空以触发自然完成 */
    std::chrono::steady_clock::time_point natural_complete_since_{};
    std::atomic<bool> completing_cleanup_{false};      /**< 自然完成清理中：忽略 TCP 断线误判终止 */
    std::atomic<bool> mcu_rates_seen_nonempty_{false}; /**< 本作业曾观测到 MCU 缓存非空（防短 G-code 初值 0 误判排空） */
    std::atomic<bool> tcp_small_mem_profile_{false};   /**< 当前是否启用小内存 TCP 参数档 */
    std::atomic<int>  tcp_pending_profile_{-1};        /**< -1=无挂起，0=legacy，1=small-mem */
    std::atomic<uint16_t> tcp_batch_size_{TCP_BATCH_SIZE}; /**< 运行时 batch 大小 */
    std::atomic<uint16_t> tcp_send_interval_ms_{TCP_SEND_INTERVAL_MS}; /**< 运行时发送轮询间隔 */
    std::atomic<uint16_t> tcp_heartbeat_interval_ms_{TCP_HEARTBEAT_INTERVAL_MS}; /**< 运行时心跳间隔 */
    std::atomic<uint8_t>  tcp_cmd_backpressure_pct_{MCU_CMD_BACKPRESSURE_PCT}; /**< 运行时背压阈值 */
    std::atomic<int64_t>  last_tcp_hb_rsp_ms_{0}; /**< 最近一次收到 TCP 状态应答的单调时刻（ms）。 */

    /* ---------- TCP 0x0F 首包固件版本缓存（About 页 Galvo Version） ---------- */
    std::atomic<bool>     tcp_fw_valid_{false};       /**< 是否已收到带版本的扩展包 */
    std::atomic<uint32_t> tcp_fw_version_le_{0};      /**< 小端固件版本号 */
    std::atomic<uint8_t>  tcp_firmware_id_{0};        /**< 1=APPA，2=APPB */
    std::atomic<bool>     mcu_fw_valid_{false};       /**< 是否已收到串口版本上报 */
    middleware::McuVersionPayload mcu_fw_cache_{};    /**< MCU 版本缓存（由 mcu_fw_mtx_ 保护） */
    mutable std::mutex mcu_fw_mtx_;
    std::atomic<bool> serial_laser_off_applied_{false}; /**< 本串口会话已发过上电 0x12 */
    std::atomic<bool> serial_laser_off_pending_{false}; /**< 版本已到但当时在打印，待空闲再发 */
    std::atomic<bool> reassert_red_after_laser_off_{false}; /**< 等串口 0x12 出队后再发 TCP 红光 */
    std::atomic<uint8_t> reassert_red_code_{1}; /**< 补发的 DATA[3]：1=红光，3=红光常亮 */
    std::atomic<uint8_t> last_requested_red_code_{0}; /**< 最近一次意图：1/2/3；2=激光，补发必须让路 */

    /* ---------- 换层/串口握手状态 ---------- */
    std::atomic<bool> waiting_layer_{false};           /**< 当前是否处于换层等待状态 */
    std::atomic<bool> layer_drain_pending_{false};     /**< 已遇到 LayerMark，等待 MCU 缓存排空后发送 layer_done */
    std::chrono::steady_clock::time_point layer_drain_since_{}; /**< 进入换层排空等待的时刻，用于兜底放行计时 */
    std::atomic<int>  serial_handshake_phase_{0};      /**< 串口握手阶段（0=无 1=等CONDITION_OK 2=等PRINT_READY） */
    std::atomic<bool> serial_evt_condition_ok_{false}; /**< 串口收到打印条件就绪事件 */
    std::atomic<bool> serial_evt_print_ready_{false};  /**< 串口收到打印准备就绪事件 */

    /* ---------- 层高信息 ---------- */
    uint16_t active_layer_height_um_ = GCODE_DEFAULT_LAYER_UM; /**< 当前/下层高度（微米）：首层握手 PRINT_START 与换层 LAYER_DONE 共用 */
    /** 收到 PRINT_READY 时的 sent_packets，用于判定本层是否已下发扫描指令 */
    int sent_at_layer_ready_ = 0;
    /** 当前层是否已在换层排空路径拍过照（避免完成时重复 Snap） */
    bool current_layer_snapped_ = false;
    bool first_layer_check_passed_ = false;

    /* ---------- 预取队列 ---------- */
    std::deque<upc::parser::rb_packet_t> tcp_stage_;      /**< 从 RingBuffer 预取出来的包暂存队列 */
    mutable std::mutex tcp_stage_mtx_;       /**< 保护 tcp_stage_ 的互斥锁 */
    std::mutex tcp_send_mtx_;                /**< 保护 TCP send 的互斥锁（避免多线程并发写 socket） */

    /* ---------- 接收累积缓冲区 ---------- */
    uint8_t tcp_rx_accum_[1024]{};   /**< TCP 接收分帧累积缓冲区 */
    size_t  tcp_rx_accum_len_ = 0;   /**< TCP 累积缓冲区当前有效长度 */
    uint8_t serial_rx_accum_[256]{}; /**< 串口接收分帧累积缓冲区 */
    size_t  serial_rx_accum_len_ = 0;/**< 串口累积缓冲区当前有效长度 */

    struct SerialTxItem {
        uint8_t data[64]{};
        size_t  len = 0;
        bool    reliable = false;  /**< 是否走带 ACK 的可靠发送通道 */
    };
    std::deque<SerialTxItem> serial_tx_queue_;
    mutable std::mutex serial_tx_mtx_;

    struct SerialReliablePending {
        bool active = false;
        uint8_t data[64]{};
        size_t  len = 0;
        serial_ack_key_t key{};
        int retry_count = 0;
        bool abort_print_on_fail = false;
        std::chrono::steady_clock::time_point deadline{};
    };
    SerialReliablePending serial_reliable_pending_{};
    std::deque<SerialReliableItem> serial_reliable_queue_;
    std::atomic<bool> serial_ack_enabled_{GLAVO_SERIAL_ACK_ENABLE != 0};
    std::atomic<float> z_pos_mm_cache_{0.0f};

    EnvSnapshotCache env_cache_{};
    mutable std::mutex env_mtx_;
    bool powder_low_latched_ = false; /**< 缺粉边沿通知闩锁：低电平持续期间只触发一次 */
    bool o2_alarm_active_ = false; /**< SOC 1120 当前是否 ACTIVE（供快照/灯效补齐） */
    AlarmSnapAssembly alarm_snap_{}; /**< MCU 快照攒帧，转发前可并入 1120 */
    std::chrono::steady_clock::time_point last_o2_wait_log_{}; /**< 起印等氧日志节流 */
    /** 本运行是否已收到过真实 MCU 1ST（关 Env Sim 后清零）；供 SOC 独立推送时区分 MCU 字段 */
    std::atomic<bool> mcu_1st_seen_{false};

    std::atomic<bool> env_sim_on_{false};  /**< 环境上报模拟（调试页开关，默认关） */
    std::atomic<bool> skip_o2_print_gate_{false}; /**< 调试：跳过 SOC 氧打印门+打印前洗气（默认关） */
    /** 本打印作业由 SOC 发过洗气开；终止/完成后据此关阀，Skip O2 路径保持 false */
    std::atomic<bool> gas_wash_opened_by_soc_{false};
    std::atomic<bool> env_sim_powder_low_target_{false}; /**< 目标缺粉态（UI 开关值） */
    bool env_sim_powder_low_effective_ = false;          /**< 已生效缺粉态（延迟后应用） */
    std::chrono::steady_clock::time_point env_sim_powder_switch_at_{}; /**< 延迟切换时刻 */
    std::chrono::steady_clock::time_point next_env_sim_tick_{};
    std::chrono::steady_clock::time_point next_soc_env_tick_{};
    float env_sim_phase_ = 0.f;
    std::atomic<int64_t> last_env_mqtt_publish_ms_{0}; /**< Glavo→MQTT 环境 IPC 最近发送时刻 */
    bool laser_power_log_valid_ = false; /**< 功率包日志去重：最近一次已打印的功率值是否有效 */
    uint16_t last_laser_power_raw_ = 0;  /**< 功率包日志去重：最近一次已打印的功率原始值 */

    std::mutex door_open_rsp_mtx_;
    std::condition_variable door_open_rsp_cv_;
    bool door_open_rsp_pending_ = false;
    bool door_open_rsp_opened_  = false;

    McuOtaSession ota_session_;
    McuSerialOtaSession serial_ota_session_;
    std::atomic<bool> ota_post_reconnect_{false};

    /* ---------- 常量 ---------- */
    static constexpr size_t kSerialTxQueueMax = 32;
    static constexpr int kMaxMotionBatch = TCP_BATCH_SIZE; /**< 单次下发的最大运动包数量 */
    static constexpr int kEnvSimIntervalMs = 1000; /**< 环境模拟刷新间隔 */
    static constexpr int kEnvSimPowderDelayMs = 5000; /**< 粉量模拟切换延迟 */
    static constexpr int kEnvMqttPublishIntervalMs = 2000; /**< Glavo→MQTT 环境 IPC 间隔 */
    static constexpr int kSocEnvPublishIntervalMs = 500; /**< V4 SOC→UI 推送间隔 */
};

} // namespace upc::glavo

#endif

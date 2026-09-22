#include "mcu_link.hpp"
#include "protocol.hpp"
#include "soc_sensors.hpp"
#include "spi_status_led.hpp"
#include "wifi_manager.hpp"
#include "print_job_state.hpp"
#include "print_video_notify.hpp"
#include "parser_manager.hpp"
#include "parser_worker.hpp"
#include "parser_context.hpp"
#include "cal_parser.hpp"
#include "galvo_coord.hpp"
#include "msg_bus.hpp"
#include "msg_runtime.hpp"
#include "rs_log.hpp"
#include "msg_port.hpp"
#include "config/cal_config.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>

#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>

namespace upc::glavo {

namespace parser = upc::parser;

namespace {

/** 1120 = alarm_snapshot_code_of(3, 20)；SOC 氧报警并进快照/灯效。 */
constexpr uint8_t kChamberOxygenAlarmSlotId = 3;
constexpr uint8_t kChamberOxygenAlarmBit = 20;
constexpr uint32_t kChamberOxygenAlarmBitMask = (1u << kChamberOxygenAlarmBit);

void publish_alarm_snapshot_ntf(const serial_alarm_snapshot_t& snap);
serial_alarm_report_t make_soc_o2_alarm_report(bool active);

bool parse_env_switch(const char* value, bool default_value)
{
    if(!value || !value[0]) {
        return default_value;
    }
    if(std::strcmp(value, "1") == 0 ||
       std::strcmp(value, "true") == 0 ||
       std::strcmp(value, "TRUE") == 0 ||
       std::strcmp(value, "on") == 0 ||
       std::strcmp(value, "ON") == 0 ||
       std::strcmp(value, "yes") == 0 ||
       std::strcmp(value, "YES") == 0) {
        return true;
    }
    if(std::strcmp(value, "0") == 0 ||
       std::strcmp(value, "false") == 0 ||
       std::strcmp(value, "FALSE") == 0 ||
       std::strcmp(value, "off") == 0 ||
       std::strcmp(value, "OFF") == 0 ||
       std::strcmp(value, "no") == 0 ||
       std::strcmp(value, "NO") == 0) {
        return false;
    }
    return default_value;
}

bool tcp_hb_rsp_log_enabled()
{
    static const bool k_enabled =
        parse_env_switch(
            std::getenv("UPC_GLAVO_LOG_TCP_HEARTBEAT_RSP"),
            UPC_GLAVO_LOG_TCP_HEARTBEAT_RSP_DEFAULT != 0);
    return k_enabled;
}

bool tcp_tx_log_enabled()
{
    static const bool k_enabled =
        parse_env_switch(
            std::getenv("UPC_GLAVO_LOG_TCP_TX"),
            UPC_GLAVO_LOG_TCP_TX_DEFAULT != 0);
    return k_enabled;
}

bool laser_power_log_enabled()
{
    static const bool k_enabled =
        parse_env_switch(
            std::getenv("UPC_GLAVO_LOG_LASER_POWER"),
            UPC_GLAVO_LOG_LASER_POWER_DEFAULT != 0);
    return k_enabled;
}

void tcp_log_tx_frame(const uint8_t* data, size_t len)
{
    if(!tcp_tx_log_enabled() || !data || len == 0) {
        return;
    }
    const bool is_tcp_frame =
        (len >= 4 && data[0] == TCP_SOF0 && data[1] == TCP_SOF1);
    const uint8_t pkg_num = (len >= 3) ? data[2] : 0u;
    const uint8_t cmd = (len >= 4) ? data[3] : 0u;
    if(is_tcp_frame && pkg_num == 0x01 && cmd == 0x0B) {
        return; // 过滤心跳帧，仅保留下发指令包日志
    }

    const unsigned frame_id =
        is_tcp_frame ? static_cast<unsigned>(cmd) : 0xFFu;
    const unsigned payload_len =
        (len >= 3) ? static_cast<unsigned>(data[2]) : 0u;
    std::string hex;
    hex.reserve(len * 3);
    for(size_t i = 0; i < len; i++) {
        char part[4];
        const int n = std::snprintf(part, sizeof(part), "%02X", data[i]);
        if(n > 0) {
            hex.append(part, static_cast<size_t>(n));
        }
        if(i + 1 < len) {
            hex.push_back(' ');
        }
    }
    UPC_LOG_MCU_LINK_TO(TcpMotionTx, Info, "TCP_TX frame[%zu] id=0x%02X payload=%u %s",
        len, frame_id, payload_len, hex.c_str());
}

parser::IParserWorker* active_parser()
{
    return parser::ParserManager::instance().active();
}

#if UPC_DEBUG && UPC_MOD_MCU_LINK

/** 串口帧十六进制转储（5A A5 AA/BB …）；单条最多展示 64 字节 */
void serial_log_frame_hex(const char* dir, const uint8_t* data, size_t len)
{
    if(!dir || !data || len == 0) {
        return;
    }
    constexpr size_t kMaxShow = 64;
    const size_t show = len > kMaxShow ? kMaxShow : len;
    char hex[320];
    size_t pos = 0;
    for(size_t i = 0; i < show && pos + 4 < sizeof(hex); i++) {
        pos += static_cast<size_t>(std::snprintf(
            hex + pos, sizeof(hex) - pos, "%02X%s",
            data[i], (i + 1 < show) ? " " : ""));
    }
    if(len > show && pos + 16 < sizeof(hex)) {
        std::snprintf(hex + pos, sizeof(hex) - pos, " ... +%zuB", len - show);
    }
    UPC_LOG_MCU_LINK(Debug, "SERIAL_%s frame[%zu] %s", dir, len, hex);
}

void serial_log_rx_frame(const uint8_t* frame, size_t len)
{
    serial_log_frame_hex("RX", frame, len);

    if(serial_is_mcu_ack(frame, len)) {
        UPC_LOG_MCU_LINK(Debug, "RX MCU_ACK port=%02X cmd=%02X type=%02X",
            len >= 4 ? frame[3] : 0,
            len >= 5 ? frame[4] : 0,
            len >= 8 ? frame[7] : 0);
        return;
    }

    serial_sensor_report_t sensor{};
    serial_sensor_err_t serr = SERIAL_SENSOR_ERR_NONE;
    if(serial_parse_sensor_report_detail(frame, len, &sensor, &serr) == 0) {
        /* 传感器数值统一由 on_sensor_report 的 Info 日志输出，避免重复打印。 */
        return;
    }

    serial_sensor_ext_t sext{};
    if(serial_parse_sensor_ext_detail(frame, len, &sext, nullptr) == 0) {
        return;
    }

    /* 运动轴位置上报（0xA5 0x9B + TLV 0x05）：与传感器帧同 Port/Cmd，需在
     * serial_frame_is_sensor_report 之前判定，否则会被误记为 RX SENSOR FAIL。 */
    serial_motion_pos_t mpos{};
    if(serial_parse_motion_pos_report(frame, len, &mpos) == 0) {
        UPC_LOG_MCU_LINK(Debug,
            "RX MOTION OK Z=%.3fmm Zwork=%.3fmm S=%.3fmm D=%.3fdeg",
            static_cast<double>(mpos.z_pos_mm), static_cast<double>(mpos.z_work_zero_mm),
            static_cast<double>(mpos.s_pos_mm), static_cast<double>(mpos.d_pos_deg));
        return;
    }

    if(serial_frame_is_sensor_report(frame, len)) {
        UPC_LOG_MCU_LINK_TO(SerialSensor, Warn, "RX SENSOR FAIL len=%zu err=%s",
            len, serial_sensor_err_string(serr));
        return;
    }

    const serial_event_t evt = serial_parse_frame(frame, len);
    if(evt != SERIAL_EVT_NONE) {
        const char* evt_name = "UNKNOWN";
        switch(evt) {
            case SERIAL_EVT_PRINT_PREPARING:    evt_name = "PRINT_PREPARING";    break;
            case SERIAL_EVT_PRINT_READY:        evt_name = "PRINT_READY";        break;
            case SERIAL_EVT_PRINT_CONDITION_OK: evt_name = "PRINT_CONDITION_OK"; break;
            default: break;
        }
        UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "RX LAYER event=%s len=%zu", evt_name, len);
        return;
    }

    uint8_t clean_result = 0;
    if(serial_parse_clean_test_result(frame, len, &clean_result) == 0) {
        UPC_LOG_MCU_LINK(Debug, "RX CLEAN_TEST_DONE result=%u",
            static_cast<unsigned>(clean_result));
        return;
    }

    serial_alarm_report_t alarm_dbg{};
    if(serial_parse_alarm_single_report(frame, len, &alarm_dbg) == 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Debug,
            "RX ALARM code=%u sev=%u state=0x%02X clear=%u detail=%u",
            static_cast<unsigned>(alarm_dbg.alarm_code),
            static_cast<unsigned>(alarm_dbg.severity),
            static_cast<unsigned>(alarm_dbg.state_mask),
            static_cast<unsigned>(alarm_dbg.clear_mode),
            static_cast<unsigned>(alarm_dbg.detail));
        return;
    }

    serial_alarm_snapshot_t snap_dbg{};
    if(serial_parse_alarm_snapshot(frame, len, &snap_dbg) == 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Debug,
            "RX ALARM SNAPSHOT slot %u (count=%u) active=0x%08X latched=0x%08X",
            static_cast<unsigned>(snap_dbg.slot_id),
            static_cast<unsigned>(snap_dbg.nonzero_slot_count),
            static_cast<unsigned>(snap_dbg.active_mask),
            static_cast<unsigned>(snap_dbg.latched_mask));
        return;
    }

    serial_alarm_reset_result_t reset_dbg{};
    if(serial_parse_alarm_reset_result(frame, len, &reset_dbg) == 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Debug, "RX ALARM RESET_RESULT code=%u result=%u",
            static_cast<unsigned>(reset_dbg.alarm_code),
            static_cast<unsigned>(reset_dbg.result));
        return;
    }

    if(len >= 3 && frame[0] == 0x5A && frame[1] == 0xA5 && frame[2] == 0xBB) {
        UPC_LOG_MCU_LINK(Debug,
            "RX SERIAL UNKNOWN len=%zu port=%02X cmd=%02X seq=%u len_field=%u",
            len,
            len >= 4 ? frame[3] : 0,
            len >= 5 ? frame[4] : 0,
            len >= 6 ? static_cast<unsigned>(frame[5]) : 0u,
            len >= 7 ? static_cast<unsigned>(frame[6]) : 0u);
        return;
    }

    UPC_LOG_MCU_LINK(Warn, "RX SERIAL BAD len=%zu", len);
}
#endif

/**
 * @brief 串口打印前握手阶段枚举
 *
 * 仅在 GLAVO_ENABLE_LAYER_CHANGE 开启时有效：
 * - NONE：无需握手或已完成
 * - WAIT_CONDITION_OK：已发打印请求，等待 MCU 打印条件就绪；
 *   氧门开启时另发洗气开并等 SOC 氧 < 阈值；Skip O2 / EnvSim / 测试图跳过洗气与等氧
 * - WAIT_PRINT_READY：已发首层/换层参数，等待 MCU 返回准备就绪
 */
enum SerialHandshakePhase : int {
    SERIAL_HS_NONE = 0,
    SERIAL_HS_WAIT_CONDITION_OK,
    SERIAL_HS_WAIT_PRINT_READY,
};

/** @brief 帧长度猜测函数类型：根据缓冲区内容猜测完整帧长度 */
using frame_guess_fn  = size_t (*)(const uint8_t*, size_t);
/** @brief 帧处理函数类型：对解析出的完整帧进行业务处理 */
using frame_handle_fn = void (*)(const uint8_t*, size_t);

struct TcpInit0fReport {
    bool has_version = false;
    uint8_t firmware_id = 0;
    uint8_t ota_state = 0;
    uint32_t fw_version_le = 0;
    uint8_t selfcheck_boot_count = 0;
};

bool parse_tcp_init_0f_report(const uint8_t* frame, size_t len,
                              TcpInit0fReport* out)
{
    if(!frame || !out || len < 4) {
        return false;
    }
    if(frame[0] != TCP_SOF0 || frame[1] != TCP_SOF1 || frame[3] != 0x0F) {
        return false;
    }

    const uint8_t payload_len = frame[2];
    if(payload_len == 0x01 && len >= 4) {
        // 旧版：仅初始化就绪信号，无版本信息。
        *out = TcpInit0fReport{};
        return true;
    }

    if(payload_len < 0x08 || len < 11) {
        return false;
    }

    out->has_version = true;
    out->firmware_id = frame[4];
    out->ota_state = frame[5];
    out->fw_version_le =
        static_cast<uint32_t>(frame[6]) |
        (static_cast<uint32_t>(frame[7]) << 8u) |
        (static_cast<uint32_t>(frame[8]) << 16u) |
        (static_cast<uint32_t>(frame[9]) << 24u);
    out->selfcheck_boot_count = frame[10];
    return true;
}

/**
 * @brief TCP 帧处理函数（MCU→SOC，Length+ID 格式）
 */
void tcp_handle_frame(const uint8_t* frame, size_t len)
{
    TcpInit0fReport init{};
    if(parse_tcp_init_0f_report(frame, len, &init)) {
        McuLink::instance().on_tcp_init_report(
            init.has_version,
            init.fw_version_le,
            init.firmware_id,
            init.ota_state,
            init.selfcheck_boot_count);
#if UPC_DEBUG && UPC_MOD_MCU_LINK
        if(init.has_version) {
            UPC_LOG_MCU_LINK(Info,
                "RX TCP link ready ext len=%zu fw_id=%u ota=0x%02X fw=0x%08X selfchk=%u",
                len,
                static_cast<unsigned>(init.firmware_id),
                static_cast<unsigned>(init.ota_state),
                static_cast<unsigned>(init.fw_version_le),
                static_cast<unsigned>(init.selfcheck_boot_count));
        } else {
            UPC_LOG_MCU_LINK(Info, "RX TCP link ready legacy len=%zu", len);
        }
#endif
        return;
    }

    if(len >= 19 && frame[0] == 0xBE && frame[1] == 0xEF &&
       frame[2] == 0x10 && frame[3] == 0x01) {
        tcp_response_t resp{};
        if(parse_tcp_response(frame, len, &resp) == 0) {
            McuLink::instance().on_tcp_status_response(resp);
        }
    }
}

/**
 * @brief 串口帧处理函数
 * @param frame 完整帧数据指针
 * @param len   帧长度
 *
 * 解析帧为串口事件，并通过 McuLink 单例分发到 notify_serial_event，
 * 驱动串口握手状态机前进。
 */
void serial_handle_frame(const uint8_t* frame, size_t len)
{
    McuLink::instance().handle_serial_rx_frame(frame, len);
}

/**
 * @brief 通用分帧接收处理函数
 * @param recv_fn    非阻塞接收函数，返回读取到的字节数（<=0 表示无数据或断开）
 * @param chunk      单次 recv 使用的临时缓冲区
 * @param chunk_sz   chunk 大小
 * @param accum      累积缓冲区（用于处理粘包/拆包）
 * @param accum_cap  累积缓冲区总容量
 * @param accum_len  累积缓冲区当前有效长度（in/out）
 * @param guess      帧长度猜测函数
 * @param handle     帧处理函数
 *
 * 逻辑：
 * 1. 循环调用 recv_fn 读取数据到 chunk；
 * 2. 将 chunk 追加到 accum 尾部；
 * 3. 对 accum 做分帧解析：调用 guess 得到帧长度，调用 handle 处理完整帧；
 * 4. 若 accum 即将溢出，丢弃前一半旧数据以腾出空间。
 */
void drain_framed_rx(int (*recv_fn)(uint8_t*, size_t),
                     uint8_t* chunk, size_t chunk_sz,
                     uint8_t* accum, size_t accum_cap, size_t* accum_len,
                     frame_guess_fn guess,
                     frame_handle_fn handle)
{
    for(;;) {
        const int n = recv_fn(chunk, chunk_sz);
        if(n <= 0) {
            break; // 无数据或出错，结束本次排空
        }
        // 防溢出：追加前必须腾出空间（旧逻辑在 accum_len 较小时会越界写）
        if((size_t)n > accum_cap) {
            std::memcpy(accum, chunk + n - accum_cap, accum_cap);
            *accum_len = accum_cap;
        } else {
            while(*accum_len + (size_t)n > accum_cap) {
                const size_t drop = *accum_len + (size_t)n - accum_cap;
                if(drop >= *accum_len) {
                    *accum_len = 0;
                    break;
                }
                std::memmove(accum, accum + drop, *accum_len - drop);
                *accum_len -= drop;
            }
            std::memcpy(accum + *accum_len, chunk, (size_t)n);
            *accum_len += (size_t)n;
        }

        // 分帧解析
        size_t pos = 0;
        while(pos < *accum_len) {
            const size_t avail = *accum_len - pos;
            const size_t flen = guess(accum + pos, avail);
            if(flen == 0) {
                break; // 数据不足以构成完整帧，保留剩余数据等待下次接收
            }
            if(flen == 1) {
                pos++; // 非法字节，跳过
                continue;
            }
            handle(accum + pos, flen);
            pos += flen;
        }
        // 移除已处理的数据，保留未解析的部分
        if(pos > 0) {
            std::memmove(accum, accum + pos, *accum_len - pos);
            *accum_len -= pos;
        }
    }
}

} // namespace

/* ---------- 单例与生命周期 ---------- */

McuLink& McuLink::instance()
{
    static McuLink inst;
    return inst;
}

McuLink::McuLink() = default;

McuLink::~McuLink()
{
    stop(); // 确保析构时线程已结束、fd 已关闭
}

/* ---------- 依赖注入与回调设置 ---------- */

void McuLink::bind_ring_buffer(parser::RingBuffer* rb)
{
    rb_ = rb;
}

void McuLink::set_job_event_callback(McuLinkEventCb cb)
{
    event_cb_ = std::move(cb);
}

void McuLink::set_progress_callback(McuLinkProgressCb cb)
{
    progress_cb_ = std::move(cb);
}

void McuLink::set_clean_test_done_callback(McuLinkCleanTestDoneCb cb)
{
    clean_test_done_cb_ = std::move(cb);
}

void McuLink::set_transport_result_callback(McuLinkTransportResultCb cb)
{
    transport_result_cb_ = std::move(cb);
}

void McuLink::set_first_layer_check_callback(McuLinkFirstLayerCheckCb cb)
{
    first_layer_check_cb_ = std::move(cb);
}

void McuLink::set_serial_ack_enabled(bool enabled)
{
    serial_ack_enabled_.store(enabled);
    if(!enabled) {
        serial_clear_reliable();
    }
}

bool McuLink::serial_ack_enabled() const noexcept
{
    return serial_ack_enabled_.load();
}

void McuLink::set_env_sim_enabled(bool on)
{
    const bool was_on = env_sim_on_.exchange(on);
    if(on && !was_on) {
        env_sim_phase_ = 0.f;
        next_env_sim_tick_ = std::chrono::steady_clock::now();
        env_sim_powder_low_target_.store(false);
        env_sim_powder_low_effective_ = false;
        env_sim_powder_switch_at_ = {};
        UPC_LOG_MCU_LINK(Info, "env_sim enabled");
    } else if(!on && was_on) {
        /* 关模拟：清缓存并推无效哨兵，UI 回 "--"；之后等真实 0x9B 再刷新。 */
        env_sim_powder_low_target_.store(false);
        env_sim_powder_low_effective_ = false;
        env_sim_powder_switch_at_ = {};
        clear_env_after_sim_disable();
        UPC_LOG_MCU_LINK(Info, "env_sim disabled, env cache cleared");
    }
}

void McuLink::clear_env_after_sim_disable()
{
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_ = EnvSnapshotCache{};
        env_cache_.pm25_indoor      = -1.0f;
        env_cache_.pm25_cabin       = -1.0f;
        env_cache_.pm1_indoor       = -1.0f;
        env_cache_.temperature_c    = -1.0f;
        env_cache_.oxygen_percent   = -1.0f;
        env_cache_.filter_oxygen_percent = -1.0f;
        env_cache_.humidity_percent = -1.0f;
        env_cache_.pressure_kpa     = -1.0f;
        env_cache_.baseplate_temp_c = -1.0f;
        env_cache_.pm10_indoor      = -1.0f;
        env_cache_.pm1_cabin        = -1.0f;
        env_cache_.pm10_cabin       = -1.0f;
        env_cache_.powder_bits      = 0u;
        env_cache_.outer_door_open  = false;
        env_cache_.door_state_valid = false;
        env_cache_.valid            = false;
        env_cache_.sensor_v4        = true;
    }
    mcu_1st_seen_.store(false);
    env_sim_phase_ = 0.f;
    next_env_sim_tick_ = {};
    next_soc_env_tick_ = {};
    publish_env_invalid_ntf();
    publish_debug_sensor_invalid_ntf();
}

bool McuLink::env_sim_enabled() const noexcept
{
    return env_sim_on_.load();
}

void McuLink::set_skip_o2_print_gate(bool on)
{
    const bool was = skip_o2_print_gate_.exchange(on);
    if(was != on) {
        UPC_LOG_MCU_LINK(Info, "skip_o2_print_gate=%d", on ? 1 : 0);
    }
}

bool McuLink::skip_o2_print_gate() const noexcept
{
    return skip_o2_print_gate_.load();
}

void McuLink::set_env_sim_powder_low(bool on)
{
    env_sim_powder_low_target_.store(on);
    if(!env_sim_on_.load()) {
        env_sim_powder_low_effective_ = false;
        env_sim_powder_switch_at_ = {};
        return;
    }
    env_sim_powder_switch_at_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kEnvSimPowderDelayMs);
    UPC_LOG_MCU_LINK(Info, "env_sim powder target=%d (apply in %dms)",
        on ? 1 : 0, kEnvSimPowderDelayMs);
}

bool McuLink::env_sim_powder_low() const noexcept
{
    return env_sim_powder_low_target_.load();
}

bool McuLink::should_publish_env_to_mqtt(bool force) noexcept
{
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if(force) {
        last_env_mqtt_publish_ms_.store(now_ms);
        return true;
    }

    int64_t last_ms = last_env_mqtt_publish_ms_.load();
    for(;;) {
        if(last_ms != 0 && now_ms - last_ms < kEnvMqttPublishIntervalMs) {
            return false;
        }
        if(last_env_mqtt_publish_ms_.compare_exchange_weak(last_ms, now_ms)) {
            return true;
        }
    }
}

/* ---------- 静态工具 ---------- */

bool McuLink::set_tcp_nodelay(int sockfd)
{
    int flag = 1;
    return setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

/* ---------- TCP 连接 ---------- */

int McuLink::connect_tcp()
{
    const char* ip = GLAVO_TCP_DEFAULT_IP;
    const int   port = GLAVO_TCP_DEFAULT_PORT;

    // 若已有连接，先关闭旧 fd
    if(tcp_fd_ >= 0) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        return -1; // 创建 socket 失败
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if(inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        return -2; // IP 地址格式错误
    }

    // 连接前即设非阻塞：避免阻塞式 connect() 卡住链路线程（含串口 TX/RX）。
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    const int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if(rc < 0) {
        if(errno != EINPROGRESS) {
            close(fd);
            return -3; // 立即失败
        }
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const int pr = ::poll(&pfd, 1, GLAVO_TCP_CONNECT_TIMEOUT_MS);
        if(pr <= 0) {
            close(fd);
            return -4; // 超时或 poll 出错
        }
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
            close(fd);
            return -5; // 连接最终失败
        }
    }

    if(!set_tcp_nodelay(fd)) {
        UPC_LOG_MCU_LINK(Warn, "TCP_NODELAY set failed");
    }

    tcp_fd_ = fd;
    tcp_rx_accum_len_ = 0; // 清空接收累积区
    last_tcp_hb_rsp_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count());

    UPC_LOG_MCU_LINK(Info, "TCP connected %s:%d (NODELAY=1)", ip, port);
    return 0;
}

/* ---------- 串口打开与配置 ---------- */

int McuLink::open_serial(const char* device, int baud)
{
    if(!device) {
        return -1;
    }
    // 关闭旧 fd
    if(serial_fd_ >= 0) {
        close(serial_fd_);
        serial_fd_ = -1;
    }

    const int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) {
        return -1; // 打开设备失败
    }

    struct termios tty{};
    if(tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1; // 获取串口属性失败
    }

    // 设置波特率
    speed_t spd = B115200;
    switch(baud) {
        case 9600:   spd = B9600; break;
        case 115200: spd = B115200; break;
        default:     spd = B115200; break;
    }
    cfsetospeed(&tty, spd);
    cfsetispeed(&tty, spd);

    // 8N1 无硬件流控，原始模式
    tty.c_cflag |= (CLOCAL | CREAD);           // 忽略调制解调器状态，启用接收
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE); // 无校验、1 位停止位
    tty.c_cflag |= CS8;                        // 8 位数据位
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN); // 原始输入（非规范模式）
    /* 完全原始输入：务必清 ICRNL/INLCR/IGNCR，否则收到的 0x0D 会被内核转成 0x0A，
     * 破坏二进制帧内容与校验和（曾导致传感器帧 TLV-len 0x0D 被收成 0x0A）。 */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR |
                     ISTRIP | INPCK | PARMRK | BRKINT | IGNBRK);
    tty.c_oflag &= ~OPOST;                     // 原始输出
    tty.c_cc[VMIN]  = 0;                       // 非阻塞读的最小字符数
    tty.c_cc[VTIME] = 0;                       // 非阻塞读的超时时间（deciseconds）

    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH); // 清空收发缓冲区

    serial_fd_ = fd;
    serial_rx_accum_len_ = 0;
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_.valid = false;
        env_cache_.door_state_valid = false;
    }

    UPC_LOG_MCU_LINK(Info, "serial open %s @ %d", device, baud);
    serial_laser_off_applied_.store(false);
    serial_laser_off_pending_.store(false);

    /* 串口刚通就补一次报警快照：单条上报只在状态变化时发，断连期间发生的变化
     * 我们全错过了，不主动问就再也拿不到。UI 侧也会在启动/进消息页等时机请求，
     * 那边有节流，重复请求不会叠加。 */
    request_alarm_snapshot();

    /* 查 MCU 工程版本：About / MQTT / OTA 用；传感器路径不看 major。 */
    {
        uint8_t vframe[16];
        const int vn = serial_build_project_info_request(vframe, sizeof(vframe));
        if(vn > 0) {
            enqueue_serial_tx(vframe, static_cast<size_t>(vn), false);
            UPC_LOG_MCU_LINK(Info, "mcu_version query TX (serial up)");
        }
    }
    return 0;
}

void McuLink::request_alarm_snapshot()
{
    /* 新请求作废未收齐的旧轮，避免混帧。 */
    reset_alarm_snap_assembly();

    uint8_t frame[16];
    const int n = serial_build_alarm_snapshot_request(frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn, "alarm_snapshot request build failed");
        return;
    }
    UPC_LOG_MCU_LINK_TO(SerialAlarm, Info, "alarm_snapshot request TX (serial up)");
    enqueue_serial_tx(frame, static_cast<size_t>(n), true);
}

void McuLink::reset_alarm_snap_assembly()
{
    alarm_snap_ = AlarmSnapAssembly{};
}

void McuLink::flush_alarm_snap_assembly()
{
    const bool o2_active = o2_alarm_active_;
    uint8_t count = alarm_snap_.count;
    uint8_t severity = alarm_snap_.severity;
    bool has_slot3 =
        (alarm_snap_.got_mask & (1u << kChamberOxygenAlarmSlotId)) != 0;

    if(o2_active) {
        if(severity < 2) {
            severity = 2;
        }
        if(!has_slot3) {
            if(count >= SERIAL_ALARM_SNAPSHOT_SLOTS) {
                UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn,
                    "alarm snapshot full (%u), cannot append SOC O2 slot3",
                    static_cast<unsigned>(count));
            } else {
                serial_alarm_snapshot_t& s =
                    alarm_snap_.slots[kChamberOxygenAlarmSlotId];
                s.slot_id = kChamberOxygenAlarmSlotId;
                s.active_mask = kChamberOxygenAlarmBitMask;
                s.latched_mask = 0;
                s.highest_severity = severity;
                alarm_snap_.got_mask |=
                    static_cast<uint16_t>(1u << kChamberOxygenAlarmSlotId);
                ++count;
                has_slot3 = true;
            }
        } else {
            alarm_snap_.slots[kChamberOxygenAlarmSlotId].active_mask |=
                kChamberOxygenAlarmBitMask;
        }
    }

    /* 同轮所有分片的 count/severity 必须一致，否则 UI 会 reject。 */
    for(uint8_t i = 0; i < SERIAL_ALARM_SNAPSHOT_SLOTS; ++i) {
        if((alarm_snap_.got_mask & (1u << i)) == 0) {
            continue;
        }
        serial_alarm_snapshot_t s = alarm_snap_.slots[i];
        s.nonzero_slot_count = count;
        s.highest_severity = severity;
        publish_alarm_snapshot_ntf(s);
        SpiStatusLed::instance().on_alarm_snapshot(s);
    }

    if(o2_active) {
        SpiStatusLed::instance().on_alarm_report(make_soc_o2_alarm_report(true));
    }

    reset_alarm_snap_assembly();
}

void McuLink::ingest_alarm_snapshot_slot(const serial_alarm_snapshot_t& snap)
{
    const auto now = std::chrono::steady_clock::now();
    if(alarm_snap_.busy &&
       now - alarm_snap_.started >
           std::chrono::milliseconds(kAlarmSnapAssembleMs)) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn,
            "alarm snapshot assemble timeout, drop incomplete round");
        reset_alarm_snap_assembly();
    }

    /* count=0：MCU 全清。若 SOC 1120 仍活跃，改发仅含 slot3 的一轮，
     * 切勿先发全零（UI 会原子清空后再拒迟到的 slot3）。 */
    if(snap.nonzero_slot_count == 0) {
        reset_alarm_snap_assembly();
        if(o2_alarm_active_) {
            serial_alarm_snapshot_t o2{};
            o2.nonzero_slot_count = 1;
            o2.slot_id = kChamberOxygenAlarmSlotId;
            o2.highest_severity = 2;
            o2.active_mask = kChamberOxygenAlarmBitMask;
            o2.latched_mask = 0;
            publish_alarm_snapshot_ntf(o2);
            SpiStatusLed::instance().on_alarm_snapshot(o2);
            SpiStatusLed::instance().on_alarm_report(make_soc_o2_alarm_report(true));
        } else {
            publish_alarm_snapshot_ntf(snap);
            SpiStatusLed::instance().on_alarm_snapshot(snap);
        }
        return;
    }

    if(snap.slot_id >= SERIAL_ALARM_SNAPSHOT_SLOTS ||
       snap.nonzero_slot_count > SERIAL_ALARM_SNAPSHOT_SLOTS) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn,
            "alarm snapshot slot/count out of range: slot=%u count=%u",
            static_cast<unsigned>(snap.slot_id),
            static_cast<unsigned>(snap.nonzero_slot_count));
        reset_alarm_snap_assembly();
        return;
    }

    if(!alarm_snap_.busy) {
        alarm_snap_.busy = true;
        alarm_snap_.started = now;
        alarm_snap_.meta_known = true;
        alarm_snap_.count = snap.nonzero_slot_count;
        alarm_snap_.severity = snap.highest_severity;
    } else if(alarm_snap_.count != snap.nonzero_slot_count ||
              alarm_snap_.severity != snap.highest_severity) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn,
            "alarm snapshot metadata mismatch, restart round");
        reset_alarm_snap_assembly();
        alarm_snap_.busy = true;
        alarm_snap_.started = now;
        alarm_snap_.meta_known = true;
        alarm_snap_.count = snap.nonzero_slot_count;
        alarm_snap_.severity = snap.highest_severity;
    }

    const uint16_t bit = static_cast<uint16_t>(1u << snap.slot_id);
    if((alarm_snap_.got_mask & bit) != 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn,
            "alarm snapshot duplicate slot %u, restart round",
            static_cast<unsigned>(snap.slot_id));
        reset_alarm_snap_assembly();
        alarm_snap_.busy = true;
        alarm_snap_.started = now;
        alarm_snap_.meta_known = true;
        alarm_snap_.count = snap.nonzero_slot_count;
        alarm_snap_.severity = snap.highest_severity;
    }

    alarm_snap_.slots[snap.slot_id] = snap;
    alarm_snap_.got_mask |= bit;
    ++alarm_snap_.got_count;

    if(alarm_snap_.got_count >= alarm_snap_.count) {
        flush_alarm_snap_assembly();
    }
}

/* ---------- 链路线程控制 ---------- */

void McuLink::start_link_thread()
{
    if(link_run_.load()) {
        return; // 已在运行
    }
    link_run_.store(true);
    if(link_th_.joinable()) {
        link_th_.join(); // 等待旧线程结束（理论上不应发生）
    }
    SocSensors::instance().start();
    link_th_ = std::thread(&McuLink::link_thread_fn, this);
}

void McuLink::stop()
{
    link_run_.store(false);
    if(link_th_.joinable()) {
        link_th_.join(); // 等待线程结束
    }
    SocSensors::instance().stop();
    // 关闭文件描述符
    if(tcp_fd_ >= 0) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }
    if(serial_fd_ >= 0) {
        close(serial_fd_);
        serial_fd_ = -1;
    }
    serial_clear_reliable();
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_ = EnvSnapshotCache{};
    }
    {
        std::lock_guard<std::mutex> lock(door_open_rsp_mtx_);
        door_open_rsp_pending_ = false;
        door_open_rsp_opened_  = false;
    }
}

/* ---------- 状态查询 ---------- */

bool McuLink::is_tcp_connected() const { return tcp_fd_ >= 0; }
bool McuLink::is_sending() const { return is_sending_.load(); }

bool McuLink::is_terminate_pending() const noexcept
{
    return terminate_pending_.load();
}

bool McuLink::is_natural_complete_pending() const noexcept
{
    return natural_complete_pending_.load();
}

bool McuLink::is_waiting_layer() const noexcept
{
    return waiting_layer_.load();
}

bool McuLink::mcu_rates_have_been_nonempty() const noexcept
{
    return mcu_rates_seen_nonempty_.load();
}

/* ---------- 事件与回调 ---------- */

void McuLink::emit_event(McuLinkEvent ev)
{
    if(event_cb_) {
        event_cb_(ev);
    }
}

void McuLink::enqueue_serial_tx(const uint8_t* data, size_t len, bool reliable)
{
    if(!data || len == 0 || len > sizeof(SerialTxItem::data)) {
        return;
    }
    if(serial_ota_session_.is_active()) {
        UPC_LOG_MCU_LINK(Warn, "drop serial TX while MCU serial OTA active");
        return;
    }
    SerialTxItem item{};
    std::memcpy(item.data, data, len);
    item.len = len;
    item.reliable = reliable;
    std::lock_guard<std::mutex> lock(serial_tx_mtx_);
    if(serial_tx_queue_.size() >= kSerialTxQueueMax) {
        serial_tx_queue_.pop_front();
    }
    serial_tx_queue_.push_back(item);
}

void McuLink::request_reassert_red_after_serial_tx(uint8_t red_code)
{
    if(red_code != 1u && red_code != 3u) {
        red_code = 1u;
    }
    last_requested_red_code_.store(red_code);
    reassert_red_code_.store(red_code);
    reassert_red_after_laser_off_.store(true);
}

void McuLink::cancel_pending_red_reassert()
{
    reassert_red_after_laser_off_.store(false);
}

EnvSnapshotCache McuLink::env_snapshot() const
{
    EnvSnapshotCache out;
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        out = env_cache_;
    }
    if(!env_sim_on_.load() && out.sensor_v4) {
        const SocEnvSample soc = SocSensors::instance().snapshot();
        if(soc.pm25_outdoor >= 0.f) {
            out.pm25_indoor = soc.pm25_outdoor;
            out.pm1_indoor = soc.pm1_outdoor;
            out.pm10_indoor = soc.pm10_outdoor;
        }
        if(soc.pm25_cabin >= 0.f) {
            out.pm25_cabin = soc.pm25_cabin;
        }
        if(soc.pm1_cabin >= 0.f) {
            out.pm1_cabin = soc.pm1_cabin;
        }
        if(soc.pm10_cabin >= 0.f) {
            out.pm10_cabin = soc.pm10_cabin;
        }
        if(soc.temperature_c >= -40.f && soc.temperature_c < 200.f) {
            out.temperature_c = soc.temperature_c;
        }
        if(soc.humidity_percent >= 0.f) {
            out.humidity_percent = soc.humidity_percent;
        }
        if(soc.pressure1_kpa > 0.f) {
            out.pressure_kpa = soc.pressure1_kpa;
        }
        if(soc.oxygen_percent >= 0.f) {
            out.oxygen_percent = soc.oxygen_percent;
        }
        if(soc.valid) {
            out.valid = true;
        }
    }
    return out;
}

middleware::McuVersionPayload McuLink::mcu_version_cached() const
{
    middleware::McuVersionPayload p{};
    if(mcu_fw_valid_.load()) {
        std::lock_guard<std::mutex> lock(mcu_fw_mtx_);
        p = mcu_fw_cache_;
    }
    return p;
}

void McuLink::publish_debug_soc_led_ntf()
{
    middleware::DebugSocLedStatePayload p{};
    p.mask = SocSensors::instance().cabin_led_mask();
    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Soc_Led_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Soc_Led_Ntf publish_async failed");
    }
}

void McuLink::arm_soc_peripherals()
{
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        if(!env_cache_.sensor_v4) {
            /* 首次启用：SOC 字段先置无效哨兵，避免默认 0 被 UI 当成真值 */
            env_cache_.pm25_indoor      = -1.0f;
            env_cache_.pm25_cabin       = -1.0f;
            env_cache_.pm1_indoor       = -1.0f;
            env_cache_.pm10_indoor      = -1.0f;
            env_cache_.pm1_cabin        = -1.0f;
            env_cache_.pm10_cabin       = -1.0f;
            env_cache_.temperature_c    = -1.0f;
            env_cache_.humidity_percent = -1.0f;
            env_cache_.pressure_kpa     = -1.0f;
            env_cache_.oxygen_percent   = -1.0f;
            if(!mcu_1st_seen_.load()) {
                env_cache_.baseplate_temp_c = -1.0f;
            }
        }
        env_cache_.sensor_v4 = true;
    }
    SpiStatusLed::instance().set_enabled(true);
    if(SocSensors::instance().ensure_cabin_leds_default_on()) {
        publish_debug_soc_led_ntf();
    }
}

void McuLink::merge_soc_env_into_cache_locked()
{
    /* env_mtx_ 已持有 */
    if(!env_cache_.sensor_v4) {
        return;
    }
    const SocEnvSample soc = SocSensors::instance().snapshot();
    if(soc.pm25_outdoor >= 0.f) {
        env_cache_.pm25_indoor = soc.pm25_outdoor;
        env_cache_.pm1_indoor = soc.pm1_outdoor;
        env_cache_.pm10_indoor = soc.pm10_outdoor;
    }
    if(soc.pm25_cabin >= 0.f) {
        env_cache_.pm25_cabin = soc.pm25_cabin;
    }
    if(soc.pm1_cabin >= 0.f) {
        env_cache_.pm1_cabin = soc.pm1_cabin;
    }
    if(soc.pm10_cabin >= 0.f) {
        env_cache_.pm10_cabin = soc.pm10_cabin;
    }
    if(soc.temperature_c >= -40.f && soc.temperature_c < 200.f) {
        env_cache_.temperature_c = soc.temperature_c;
    }
    if(soc.humidity_percent >= 0.f) {
        env_cache_.humidity_percent = soc.humidity_percent;
    }
    if(soc.pressure1_kpa > 0.f) {
        env_cache_.pressure_kpa = soc.pressure1_kpa;
    }
    if(soc.oxygen_percent >= 0.f) {
        env_cache_.oxygen_percent = soc.oxygen_percent;
    }
}

void McuLink::on_sensor_ext_report(const serial_sensor_ext_t& ext)
{
    UPC_LOG_MCU_LINK_TO(SerialSensor, Info,
        "sensor_ext: sieve=%umA door=%umA fan_rpm=%u fault=%u state=%u link=%u",
        static_cast<unsigned>(ext.sieve_current_ma),
        static_cast<unsigned>(ext.door_coil_ma),
        static_cast<unsigned>(ext.fan_rpm),
        static_cast<unsigned>(ext.fan_fault),
        static_cast<unsigned>(ext.fan_state),
        static_cast<unsigned>((ext.fan_status_bits >> 7) & 1u));
    /* 电气量暂无消费方，仅日志；风机部分推调试页 */
    publish_debug_fan_ntf(ext);
}

/**
 * @brief 调试页风扇卡：推 MCU 实测转速/故障/状态。
 * @note bit7=0（驱动链路异常）时 MCU 把转速等字段填 0，这里原样透传并置
 *       valid=0，由 UI 显示 "--"；夹成 0 会被误读成「风机停了」。
 */
void McuLink::publish_debug_fan_ntf(const serial_sensor_ext_t& ext)
{
    middleware::DebugFanStatePayload p{};
    p.valid     = static_cast<uint8_t>((ext.fan_status_bits >> 7) & 1u);
    p.fault     = ext.fan_fault;
    p.state     = ext.fan_state;
    p.rpm       = ext.fan_rpm;
    p.bus_v_x10 = ext.fan_bus_v_x10;
    p.bus_i_ma  = ext.fan_bus_i_ma;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Fan_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Fan_Ntf publish_async failed");
    }
}

void McuLink::on_sensor_report(const serial_sensor_report_t& report)
{
    /* 模拟开启时跳过真实串口写缓存/上报，避免覆盖测试数据。 */
    if(env_sim_on_.load()) {
        return;
    }

    mcu_1st_seen_.store(true);

    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_.sensor_v4 = true;
        env_cache_.baseplate_temp_c = report.substrate_temp_c;
        env_cache_.filter_pressure_kpa =
            static_cast<float>(report.filter_pressure_x10_kpa) / 10.0f;
        env_cache_.outer_door_open = (report.outer_door_open != 0);
        env_cache_.door_state_valid = true;
        env_cache_.valid = true;

        env_cache_.powder_ir_bits = report.powder_ir_bits;
        /* UI 只看 bit0/bit1：左/右匣仍有粉（非 OUT 缺粉） */
        env_cache_.powder_bits = static_cast<uint8_t>(
            ((report.powder_left_low == 0) ? 0x01u : 0u) |
            ((report.powder_right_low == 0) ? 0x02u : 0u));
        /* 1ST oxygen_percent = 滤芯氧；舱内氧由 SOC 填 */
        env_cache_.filter_oxygen_percent = report.oxygen_percent;
        merge_soc_env_into_cache_locked();
    }
    publish_env_update_ntf();
    publish_debug_sensor_ntf(report);

    const bool powder_low =
        (report.powder_left_low != 0) || (report.powder_right_low != 0);
    if(powder_low) {
        if(!powder_low_latched_) {
            powder_low_latched_ = true;
            emit_event(McuLinkEvent::PowderLow);
        }
    } else {
        powder_low_latched_ = false;
    }

    UPC_LOG_MCU_LINK_TO(SerialSensor, Info,
        "sensor: O2=%.1f%% SCfilter=%.1fkPa base=%.1fC envNTC=%.1fC "
        "filterNTC=%.1fC sw=0x%02X ir=0x%02X door_open=%d powder_L/R_low=%d/%d",
        static_cast<double>(report.oxygen_percent),
        static_cast<double>(report.filter_pressure_x10_kpa) / 10.0,
        static_cast<double>(report.substrate_temp_c),
        static_cast<double>(report.env_ntc_c),
        static_cast<double>(report.filter_ntc_c),
        static_cast<unsigned>(report.local_switch_bits),
        static_cast<unsigned>(report.powder_ir_bits),
        report.outer_door_open ? 1 : 0,
        report.powder_left_low ? 1 : 0,
        report.powder_right_low ? 1 : 0);
}

bool McuLink::request_open_door_and_wait(uint32_t timeout_ms, bool* opened_out)
{
    if(serial_fd_ < 0) {
        UPC_LOG_MCU_LINK(Warn, "request_open_door: serial offline");
        return false;
    }

    uint8_t frame[32];
    const int n = serial_build_door_open_request(frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_MCU_LINK(Warn, "request_open_door: build frame failed");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(door_open_rsp_mtx_);
        door_open_rsp_pending_ = false;
        door_open_rsp_opened_  = false;
    }

    enqueue_serial_tx(frame, static_cast<size_t>(n), true);

    const uint32_t wait_ms = timeout_ms > 0 ? timeout_ms : 1u;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    bool opened = false;
    std::unique_lock<std::mutex> lk(door_open_rsp_mtx_);
    const bool got = door_open_rsp_cv_.wait_until(lk, deadline, [this]() {
        return door_open_rsp_pending_;
    });
    if(got) {
        opened = door_open_rsp_opened_;
        door_open_rsp_pending_ = false;
    }
    lk.unlock();

    if(opened_out) {
        *opened_out = opened;
    }
    if(!got) {
        UPC_LOG_MCU_LINK(Warn, "request_open_door: timeout %u ms", wait_ms);
    }
    return got;
}

bool McuLink::request_open_door_once()
{
    if(serial_fd_ < 0) {
        UPC_LOG_MCU_LINK(Warn, "request_open_door_once: serial offline");
        return false;
    }

    uint8_t frame[32];
    const int n = serial_build_door_open_request(frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_MCU_LINK(Warn, "request_open_door_once: build frame failed");
        return false;
    }

    enqueue_serial_tx(frame, static_cast<size_t>(n), true);
    return true;
}

bool McuLink::request_transport_once()
{
    if(serial_fd_ < 0) {
        UPC_LOG_MCU_LINK(Warn, "request_transport_once: serial offline");
        return false;
    }

    uint8_t frame[32];
    const int n = serial_build_transport_request(frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_MCU_LINK(Warn, "request_transport_once: build frame failed");
        return false;
    }

    enqueue_serial_tx(frame, static_cast<size_t>(n), true);
    UPC_LOG_MCU_LINK(Info, "transport request sent");
    return true;
}

void McuLink::drain_serial_tx_queue()
{
    if(serial_fd_ < 0) {
        return;
    }
    /* 擦除窗及整个串口 OTA 会话期间不发非 OTA 帧；OTA 走 serial_send_raw。 */
    if(serial_ota_session_.is_active()) {
        return;
    }
    for(;;) {
        SerialTxItem item{};
        {
            std::lock_guard<std::mutex> lock(serial_tx_mtx_);
            if(serial_tx_queue_.empty()) {
                break;
            }
            item = serial_tx_queue_.front();
            serial_tx_queue_.pop_front();
        }
        /* reliable 项走带 ACK 的可靠通道（本函数在 link 线程内调用，
         * 与 serial_reliable_* 状态机同线程，无需额外加锁） */
        if(item.reliable) {
            serial_send_aa_reliable(item.data, item.len, false);
        } else {
            serial_send_raw(item.data, item.len);
        }
    }
}

void McuLink::on_motion_pos_report(const serial_motion_pos_t& pos)
{
    z_pos_mm_cache_.store(pos.z_pos_mm, std::memory_order_relaxed);
    publish_debug_axis_ntf(pos);        /* 调试页三轴位置 + Z 工作坐标回读 */

    UPC_LOG_MCU_LINK(Info,
        "motion: Z=%.3fmm Zwork=%.3fmm S=%.3fmm D=%.3fdeg",
        static_cast<double>(pos.z_pos_mm), static_cast<double>(pos.z_work_zero_mm),
        static_cast<double>(pos.s_pos_mm), static_cast<double>(pos.d_pos_deg));
}


namespace {

/*
    * 激光器 3ST 扩展上报（TLV 0x04，Len 11）：在 2ST 基础上带 laser_type 与电流。
    * 预编译 protocol 静态库当前仅解析 2ST（TLV 0x03），故 3ST 在应用层就地解析。
    * 帧：5A A5 BB | Port(0xA5) Cmd(0x9B) Seq | PayloadLen(0x0D) | TLV(0x04) TlvLen(0x0B) | Value(11) | Sum
    * Value：[0]=laser_type [1..2]=status [3..4]=alarm [5..6]=power [7..8]=temperature [9..10]=current（均小端）。
    */
constexpr uint8_t kSdbgCmdSensorReport   = 0x9B;  /* CMD_SNESOR_VALUE_REPORT */
constexpr uint8_t kSdbgTlvLaserReport3St = 0x04;  /* TLV_SENSOR_REPORT_3ST */

struct LaserReport3 {
    uint8_t  type;        /**< 1=JFSC 2=AL 200W */
    uint16_t status;
    uint16_t alarm;
    uint16_t power_raw;
    uint16_t temp_raw;
    uint16_t current_raw;
};

inline uint16_t laser3_rd_le16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                    (static_cast<uint16_t>(p[1]) << 8u));
}

/** 解析 3ST 激光扩展上报；成功返回 0，非匹配/校验失败返回负值。 */
int parse_laser_report3(const uint8_t* d, size_t len, LaserReport3* out)
{
    if(!d || !out || len < 9) {
        return -1;
    }
    if(d[0] != SERIAL_SOF0 || d[1] != SERIAL_SOF1 || d[2] != SERIAL_FT_MCU_TO_SOC) {
        return -2;
    }
    if(d[3] != SDBG_PORT_SENSORS || d[4] != kSdbgCmdSensorReport) {
        return -3;
    }
    const uint8_t payload_len = d[6];
    if(payload_len != 0x0D || d[7] != kSdbgTlvLaserReport3St || d[8] != 0x0B) {
        return -4;
    }
    if(len < static_cast<size_t>(8 + payload_len)) {
        return -5;
    }
    if(serial_frame_checksum_check(d, len, nullptr, nullptr) != 0) {
        return -6;
    }
    const uint8_t* v = &d[9];
    out->type        = v[0];
    out->status      = laser3_rd_le16(v + 1);
    out->alarm       = laser3_rd_le16(v + 3);
    out->power_raw   = laser3_rd_le16(v + 5);
    out->temp_raw    = laser3_rd_le16(v + 7);
    out->current_raw = laser3_rd_le16(v + 9);
    return 0;
}

/** 组装并异步下发 GLAVO_Debug_Laser_Ntf（含型号与电流）。 */
void publish_laser_ntf(uint8_t type, uint16_t status, uint16_t alarm,
                        uint16_t power_raw, uint16_t temp_raw, uint16_t current_raw)
{
    middleware::DebugLaserStatePayload p{};
    p.status        = status;
    p.alarm         = alarm;
    p.power_raw     = power_raw;
    p.pump_temp_raw = temp_raw;
    p.current_raw   = current_raw;
    p.laser_type    = type;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Laser_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Laser_Ntf publish_async failed");
    }
}

/** 组装并异步下发 GLAVO_Alarm_Report_Ntf：TLV 字段原样透传，不查字典不加工
 *  （severity/clear_mode 以报文为准，生命周期由 UI 侧按 state_mask 推导）。 */
void publish_alarm_report_ntf(const serial_alarm_report_t& report)
{
    middleware::AlarmReportPayload p{};
    p.alarm_code = report.alarm_code;
    p.severity   = report.severity;
    p.state_mask = report.state_mask;
    p.clear_mode = report.clear_mode;
    p.detail     = report.detail;

    middleware::Message ntf(middleware::MsgType::GLAVO_Alarm_Report_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.priority = middleware::MsgPriority::High;
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Alarm_Report_Ntf publish_async failed");
    }
}

/** 组装并异步下发 GLAVO_Alarm_Snapshot_Ntf：快照的一个 slot。 */
void publish_alarm_snapshot_ntf(const serial_alarm_snapshot_t& snap)
{
    middleware::AlarmSnapshotPayload p{};
    p.nonzero_slot_count = snap.nonzero_slot_count;
    p.slot_id            = snap.slot_id;
    p.highest_severity   = snap.highest_severity;
    p.active_mask        = snap.active_mask;
    p.latched_mask       = snap.latched_mask;

    middleware::Message ntf(middleware::MsgType::GLAVO_Alarm_Snapshot_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Alarm_Snapshot_Ntf publish_async failed");
    }
}

serial_alarm_report_t make_soc_o2_alarm_report(bool active)
{
    serial_alarm_report_t o2{};
    o2.alarm_code = static_cast<uint16_t>(GLAVO_ALARM_CODE_CHAMBER_OXYGEN_HIGH);
    o2.severity = 2;
    o2.state_mask = active ? SERIAL_ALARM_STATE_ACTIVE : 0;
    o2.clear_mode = SERIAL_ALARM_CLEAR_AUTO;
    o2.detail = 0;
    return o2;
}

/** 组装并异步下发 GLAVO_Alarm_Reset_Result_Ntf：复位失败回执。
 *  MCU 只在失败时发，成功以后续 NORMAL 单报为准；UI 收到即解 reset_pending。 */
void publish_alarm_reset_result_ntf(const serial_alarm_reset_result_t& res)
{
    middleware::AlarmResetResultPayload p{};
    p.alarm_code = res.alarm_code;
    p.result     = res.result;

    middleware::Message ntf(middleware::MsgType::GLAVO_Alarm_Reset_Result_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.priority = middleware::MsgPriority::High;
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Alarm_Reset_Result_Ntf publish_async failed");
    }
}

} // namespace

void McuLink::publish_env_update_ntf()
{
    const EnvSnapshotCache snap = env_snapshot();
    if(!snap.valid) {
        return;
    }

    const middleware::UserHomeStatus status = is_tcp_connected()
        ? middleware::UserHomeStatus::Ready
        : middleware::UserHomeStatus::Idle;

    middleware::EnvSnapshotPayload p{};
    std::snprintf(p.device_name, sizeof(p.device_name), "%s", upc::config::kDeviceDisplayName);
    p.status         = static_cast<uint32_t>(status);

    /* WiFi 图标状态取 ConnMan 真实值（首页与设置页共用同一数据源）。 */
    const WifiSnapshot wifi = WifiManager::instance().snapshot();
    p.wifi_connected = wifi.connected ? 1 : 0;
    p.signal_bars    = wifi.signal_bars;

    p.pm25_indoor    = snap.pm25_indoor;
    p.pm25_cabin     = snap.pm25_cabin;
    p.temperature_c  = snap.temperature_c;
    p.oxygen_percent = snap.oxygen_percent;
    p.pm1_indoor     = snap.pm1_indoor;
    p.humidity_percent = snap.humidity_percent;
    p.pressure_kpa   = snap.pressure_kpa;
    p.baseplate_temp_c = snap.baseplate_temp_c;
    p.powder_bits    = static_cast<uint8_t>(snap.powder_bits & 0x03u);
    p.pm10_indoor    = snap.pm10_indoor;
    p.sensor_v4      = 1u;
    p.pm1_cabin      = snap.pm1_cabin;
    p.pm10_cabin     = snap.pm10_cabin;
    /* 尚未收到 MCU 1ST：基板/粉量仍属 MCU；舱氧已由 SOC 填 */
    if(!mcu_1st_seen_.load()) {
        p.baseplate_temp_c = -1.0f;
        p.powder_bits      = 0u;
    }

    middleware::Message ntf_ui(middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf_ui.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf_ui)) {
        UPC_LOG_MCU_LINK(Warn, "APP_Update_Ntf publish_async failed target=UI");
    }

    /* UI 每次采集实时刷新；Glavo→MQTT 环境 IPC 最多每 2 秒发送一次最新快照。 */
    if(should_publish_env_to_mqtt()) {
        middleware::Message ntf_mqtt(middleware::MsgType::APP_Update_Ntf,
            middleware::ModuleId::APP_GLAVO,
            middleware::ModuleId::APP_MQTT);
        ntf_mqtt.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        if(!middleware::MsgPort::instance().publish_async(ntf_mqtt)) {
            UPC_LOG_MCU_LINK(Warn, "APP_Update_Ntf publish_async failed target=APP_MQTT");
        }
    }
}

void McuLink::publish_env_status_ntf_ui_only()
{
    const middleware::UserHomeStatus status = is_tcp_connected()
        ? middleware::UserHomeStatus::Ready
        : middleware::UserHomeStatus::Idle;

    middleware::EnvSnapshotPayload p{};
    std::snprintf(p.device_name, sizeof(p.device_name), "%s", upc::config::kDeviceDisplayName);
    p.status = static_cast<uint32_t>(status);
    const WifiSnapshot wifi = WifiManager::instance().snapshot();
    p.wifi_connected = wifi.connected ? 1 : 0;
    p.signal_bars = wifi.signal_bars;

    const EnvSnapshotCache snap = env_snapshot();
    if(snap.valid) {
        p.pm25_indoor = snap.pm25_indoor;
        p.pm25_cabin = snap.pm25_cabin;
        p.temperature_c = snap.temperature_c;
        p.oxygen_percent = snap.oxygen_percent;
        p.pm1_indoor = snap.pm1_indoor;
        p.humidity_percent = snap.humidity_percent;
        p.pressure_kpa = snap.pressure_kpa;
        p.baseplate_temp_c = snap.baseplate_temp_c;
        p.pm10_indoor = snap.pm10_indoor;
        p.powder_bits = static_cast<uint8_t>(snap.powder_bits & 0x03u);
        p.sensor_v4 = 1u;
        p.pm1_cabin = snap.pm1_cabin;
        p.pm10_cabin = snap.pm10_cabin;
    } else {
        p.pm25_indoor = -1.0f;
        p.pm25_cabin = -1.0f;
        p.temperature_c = -1.0f;
        p.oxygen_percent = -1.0f;
        p.pm1_indoor = -1.0f;
        p.humidity_percent = -1.0f;
        p.pressure_kpa = -1.0f;
        p.baseplate_temp_c = -1.0f;
        p.pm10_indoor = -1.0f;
        p.powder_bits = 0u;
        p.sensor_v4 = 1u;
        p.pm1_cabin = -1.0f;
        p.pm10_cabin = -1.0f;
    }

    middleware::Message ntf_ui(middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf_ui.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf_ui)) {
        UPC_LOG_MCU_LINK(Warn, "APP_Update_Ntf(status_only) publish_async failed target=UI");
    }
}

void McuLink::publish_env_invalid_ntf()
{
    const middleware::UserHomeStatus status = is_tcp_connected()
        ? middleware::UserHomeStatus::Ready
        : middleware::UserHomeStatus::Idle;

    middleware::EnvSnapshotPayload p{};
    std::snprintf(p.device_name, sizeof(p.device_name), "%s", upc::config::kDeviceDisplayName);
    p.status           = static_cast<uint32_t>(status);
    const WifiSnapshot wifi = WifiManager::instance().snapshot();
    p.wifi_connected   = wifi.connected ? 1 : 0;
    p.signal_bars      = wifi.signal_bars;
    /* 与上电默认一致：环境指标哨兵 <0 → UI "--"；MQTT 侧据此清 has_value。 */
    p.pm25_indoor      = -1.0f;
    p.pm25_cabin       = -1.0f;
    p.temperature_c    = -1.0f;
    p.oxygen_percent   = -1.0f;
    p.pm1_indoor       = -1.0f;
    p.pm10_indoor      = -1.0f;
    p.humidity_percent = -1.0f;
    p.pressure_kpa     = -1.0f;
    p.baseplate_temp_c = -1.0f;
    p.powder_bits      = 0u;
    p.sensor_v4        = 1u;
    p.pm1_cabin        = -1.0f;
    p.pm10_cabin       = -1.0f;

    middleware::Message ntf_ui(middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf_ui.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf_ui)) {
        UPC_LOG_MCU_LINK(Warn, "APP_Update_Ntf(invalid) publish_async failed target=UI");
    }

    /* 清空帧必须立即到达 MQTT，并以当前时刻重新开始 2 秒节流窗口。 */
    if(should_publish_env_to_mqtt(true)) {
        middleware::Message ntf_mqtt(middleware::MsgType::APP_Update_Ntf,
            middleware::ModuleId::APP_GLAVO,
            middleware::ModuleId::APP_MQTT);
        ntf_mqtt.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        if(!middleware::MsgPort::instance().publish_async(ntf_mqtt)) {
            UPC_LOG_MCU_LINK(Warn, "APP_Update_Ntf(invalid) publish_async failed target=APP_MQTT");
        }
    }
}

void McuLink::publish_debug_sensor_ntf(const serial_sensor_report_t& report)
{
    middleware::DebugSensorPayload p{};
    p.oxygen_percent   = -1.0f;
    p.baseplate_temp_c = report.substrate_temp_c;
    p.powder_status    = report.powder_present
        ? static_cast<uint8_t>(middleware::DebugPowderStatus::Available)
        : static_cast<uint8_t>(middleware::DebugPowderStatus::Fault);
    /* 1ST 的 oxygen 即滤芯氧；舱内氧改用 SOC */
    p.filter_oxygen_percent = report.oxygen_percent;
    const SocEnvSample soc = SocSensors::instance().snapshot();
    p.oxygen_percent = (soc.oxygen_percent >= 0.f) ? soc.oxygen_percent : -1.f;
    p.pressure_kpa = (soc.pressure1_kpa > 0.f)
        ? soc.pressure1_kpa
        : (static_cast<float>(report.filter_pressure_x10_kpa) / 10.0f);
    p.pm25 = (soc.pm25_outdoor >= 0.f) ? soc.pm25_outdoor : -1.f;
    p.pm25_cabin = (soc.pm25_cabin >= 0.f) ? soc.pm25_cabin : -1.f;
    p.humidity_pct = soc.humidity_percent;
    p.cabin_temp_c = soc.temperature_c;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Sensor_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Sensor_Ntf publish_async failed");
    }
}

void McuLink::publish_debug_sensor_invalid_ntf()
{
    middleware::DebugSensorPayload p{};
    p.oxygen_percent   = -1.0f;
    p.filter_oxygen_percent = -1.0f;
    p.pressure_kpa     = -1.0f;
    p.pm25             = -1.0f;
    p.humidity_pct     = -1.0f;
    p.cabin_temp_c     = -1.0f;
    p.baseplate_temp_c = -1.0f;
    p.pm25_cabin       = -1.0f;
    /* 与数值哨兵配套：UI 对 powder_status==0xFF 显示 "--" */
    p.powder_status    = 0xFF;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Sensor_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Sensor_Ntf(invalid) publish_async failed");
    }
}

void McuLink::publish_debug_sensor_from_soc_and_cache()
{
    EnvSnapshotCache snap;
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        snap = env_cache_;
    }
    const SocEnvSample soc = SocSensors::instance().snapshot();

    middleware::DebugSensorPayload p{};
    if(soc.oxygen_percent >= 0.f) {
        p.oxygen_percent = soc.oxygen_percent;
    } else {
        p.oxygen_percent = -1.0f;
    }
    p.filter_oxygen_percent = snap.filter_oxygen_percent;
    if(mcu_1st_seen_.load()) {
        p.baseplate_temp_c = snap.baseplate_temp_c;
        /* powder_bits：两匣都有粉 → Available；否则 Fault */
        p.powder_status = ((snap.powder_bits & 0x03u) == 0x03u)
            ? static_cast<uint8_t>(middleware::DebugPowderStatus::Available)
            : static_cast<uint8_t>(middleware::DebugPowderStatus::Fault);
    } else {
        p.baseplate_temp_c = -1.0f;
        p.powder_status    = 0xFF;
    }

    p.pm25 = (soc.pm25_outdoor >= 0.f) ? soc.pm25_outdoor : -1.f;
    p.pm25_cabin = (soc.pm25_cabin >= 0.f) ? soc.pm25_cabin : -1.f;
    p.humidity_pct = soc.humidity_percent;
    p.cabin_temp_c = soc.temperature_c;
    if(soc.pressure1_kpa > 0.f) {
        p.pressure_kpa = soc.pressure1_kpa;
    } else if(snap.filter_pressure_kpa > 0.f) {
        p.pressure_kpa = snap.filter_pressure_kpa;
    } else {
        p.pressure_kpa = -1.0f;
    }

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Sensor_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Sensor_Ntf(soc) publish_async failed");
    }
}

void McuLink::poll_soc_env_tick()
{
    if(env_sim_on_.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if(next_soc_env_tick_.time_since_epoch().count() != 0 && now < next_soc_env_tick_) {
        return;
    }
    next_soc_env_tick_ = now + std::chrono::milliseconds(kSocEnvPublishIntervalMs);

    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_.sensor_v4 = true;
        merge_soc_env_into_cache_locked();
    }
    {
        const SocEnvSample soc = SocSensors::instance().snapshot();
        if(soc.valid) {
            std::lock_guard<std::mutex> lock(env_mtx_);
            env_cache_.valid = true;
        }
    }

    /* 首页/控制页：有任一有效源才推；调试顶栏每拍都推（无数则 "--"） */
    publish_env_update_ntf();
    publish_debug_sensor_from_soc_and_cache();
}

void McuLink::poll_env_sim_tick()
{
    if(!env_sim_on_.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if(next_env_sim_tick_.time_since_epoch().count() != 0 && now < next_env_sim_tick_) {
        return;
    }
    next_env_sim_tick_ = now + std::chrono::milliseconds(kEnvSimIntervalMs);

    const bool powder_target = env_sim_powder_low_target_.load();
    if(env_sim_powder_low_effective_ != powder_target &&
       env_sim_powder_switch_at_.time_since_epoch().count() != 0 &&
       now >= env_sim_powder_switch_at_) {
        env_sim_powder_low_effective_ = powder_target;
        env_sim_powder_switch_at_ = {};
        UPC_LOG_MCU_LINK(Info, "env_sim powder effective=%d",
            env_sim_powder_low_effective_ ? 1 : 0);
    }

    env_sim_phase_ += 0.35f;
    if(env_sim_phase_ > 6.2831853f) {
        env_sim_phase_ -= 6.2831853f;
    }

    const float s1 = static_cast<float>(std::sin(static_cast<double>(env_sim_phase_)));
    const float s2 = static_cast<float>(
        std::sin(static_cast<double>(env_sim_phase_ * 0.7f)));
    const float s3 = static_cast<float>(
        std::sin(static_cast<double>(env_sim_phase_ * 1.3f)));

    const float pm25 = 10.0f + 40.0f * (0.5f + 0.5f * s3);
    const float pm1 = pm25 * 0.6f;
    const float pm10 = pm25 * 1.8f;
    const float oxygen = 20.5f + 1.5f * s1;
    const float temperature = 25.0f + 5.0f * s2;
    const float humidity = 45.0f + 15.0f * s3;
    const float pressure_kpa = (1013.0f + 20.0f * s1) / 10.0f;
    const float filter_kpa = (1005.0f + 15.0f * s2) / 10.0f;
    const float pressure2_kpa = (1011.0f + 15.0f * s1) / 10.0f;
    const float baseplate = 30.0f + 8.0f * s2;
    const uint8_t powder_bits = env_sim_powder_low_effective_ ? 0x00u : 0x03u;
    const uint8_t door_open = (s2 > 0.75f) ? 1u : 0u;
    const uint16_t hall = static_cast<uint16_t>(
        ((s1 > 0.4f) ? 0x0001u : 0u) |
        ((s2 > 0.4f) ? 0x0002u : 0u) |
        ((s3 > 0.4f) ? 0x0004u : 0u));

    /* 模拟数据自己填舱内外，不叠真实 SOC。mcu_1st_seen 避免发布时把基板/粉量打成哨兵。 */
    mcu_1st_seen_.store(true);
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_.sensor_v4        = true;
        env_cache_.pm25_cabin       = pm25;
        env_cache_.pm25_indoor      = pm25;
        env_cache_.pm1_indoor       = pm1;
        env_cache_.pm10_indoor      = pm10;
        env_cache_.pm1_cabin        = pm1;
        env_cache_.pm10_cabin       = pm10;
        env_cache_.temperature_c    = temperature;
        env_cache_.oxygen_percent   = oxygen;
        env_cache_.filter_oxygen_percent = oxygen;
        env_cache_.humidity_percent = humidity;
        env_cache_.pressure_kpa     = pressure_kpa;
        env_cache_.baseplate_temp_c = baseplate;
        env_cache_.powder_bits      = powder_bits;
        env_cache_.outer_door_open  = (door_open != 0);
        env_cache_.door_state_valid = true;
        env_cache_.valid            = true;
    }

    publish_env_update_ntf();

    middleware::DebugSensorPayload dbg{};
    dbg.oxygen_percent = oxygen;
    dbg.filter_oxygen_percent = oxygen;
    dbg.pressure_kpa = pressure_kpa;
    dbg.pm25 = pm25;
    dbg.pm25_cabin = pm25;
    dbg.humidity_pct = humidity;
    dbg.cabin_temp_c = temperature;
    dbg.baseplate_temp_c = baseplate;
    dbg.powder_status = (powder_bits == 0x03u)
        ? static_cast<uint8_t>(middleware::DebugPowderStatus::Available)
        : static_cast<uint8_t>(middleware::DebugPowderStatus::Fault);
    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Sensor_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&dbg), sizeof(dbg));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Sensor_Ntf(env_sim) publish_async failed");
    }

    const bool powder_low = (powder_bits == 0x00u);
    if(powder_low) {
        if(!powder_low_latched_) {
            powder_low_latched_ = true;
            emit_event(McuLinkEvent::PowderLow);
        }
    } else {
        powder_low_latched_ = false;
    }

    UPC_LOG_MCU_LINK_TO(SerialSensor, Info,
        "env_sim: O2=%.1f%% P1=%.1fkPa P2=%.1fkPa PF=%.1fkPa T=%.1fC RH=%.1f%% PM1.0=%.1f PM2.5=%.1f PM10=%.1f base=%.1fC powder_bits=0x%02X door=%u hall=0x%04X",
        static_cast<double>(oxygen),
        static_cast<double>(pressure_kpa),
        static_cast<double>(pressure2_kpa),
        static_cast<double>(filter_kpa),
        static_cast<double>(temperature),
        static_cast<double>(humidity),
        static_cast<double>(pm1),
        static_cast<double>(pm25),
        static_cast<double>(pm10),
        static_cast<double>(baseplate),
        static_cast<unsigned>(powder_bits),
        static_cast<unsigned>(door_open),
        static_cast<unsigned>(hall));
}

void McuLink::publish_debug_axis_ntf(const serial_motion_pos_t& pos)
{
    middleware::DebugAxisStatePayload p{};
    p.z_pos_mm       = pos.z_pos_mm;
    p.s_pos_mm       = pos.s_pos_mm;
    p.d_pos_deg      = pos.d_pos_deg;
    p.z_work_zero_mm = pos.z_work_zero_mm;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Axis_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "GLAVO_Debug_Axis_Ntf publish_async failed");
    }
}

void McuLink::on_mcu_version_report(const serial_project_info_t& info)
{
    middleware::McuVersionPayload p{};
    p.valid        = 1;
    p.major        = info.major;
    p.minor        = info.minor;
    p.patch        = info.patch;
    p.laser_type   = info.laser_type;
    p.z_motor_type = info.z_motor_type;
    {
        std::lock_guard<std::mutex> lock(mcu_fw_mtx_);
        mcu_fw_cache_ = p;
    }
    mcu_fw_valid_.store(true);
    publish_mcu_version_ntf();
    apply_serial_laser_off_after_mcu_ready();
}

void McuLink::publish_mcu_version_ntf()
{
    middleware::McuVersionPayload p{};
    if(mcu_fw_valid_.load()) {
        std::lock_guard<std::mutex> lock(mcu_fw_mtx_);
        p = mcu_fw_cache_;
    } else {
        p.valid = 0;
    }

    auto send_to = [&](middleware::ModuleId target) {
        middleware::Message ntf(middleware::MsgType::GLAVO_Mcu_Version_Ntf,
            middleware::ModuleId::APP_GLAVO, target);
        ntf.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        if(!middleware::MsgPort::instance().publish_async(ntf)) {
            UPC_LOG_MCU_LINK(Warn, "GLAVO_Mcu_Version_Ntf publish_async failed target=%u",
                static_cast<unsigned>(target));
        }
    };
    send_to(middleware::ModuleId::UI);
    send_to(middleware::ModuleId::APP_MQTT);
}

void McuLink::notify_serial_event(serial_event_t evt)
{
    const char* evt_name = "UNKNOWN";
    switch(evt) {
        case SERIAL_EVT_PRINT_PREPARING:    evt_name = "PRINT_PREPARING";    break;
        case SERIAL_EVT_PRINT_CONDITION_OK: evt_name = "PRINT_CONDITION_OK"; break;
        case SERIAL_EVT_PRINT_READY:        evt_name = "PRINT_READY";        break;
        default:                            evt_name = "NONE";               break;
    }
    UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "notify_serial_event: %s", evt_name);
    if(evt == SERIAL_EVT_PRINT_PREPARING) {
        return;
    }
    if(evt == SERIAL_EVT_PRINT_CONDITION_OK) {
        serial_evt_condition_ok_.store(true);
    } else if(evt == SERIAL_EVT_PRINT_READY) {
        serial_evt_print_ready_.store(true);
    }
}

void McuLink::handle_serial_rx_frame(const uint8_t* frame, size_t len)
{
#if UPC_DEBUG && UPC_MOD_MCU_LINK
    serial_log_rx_frame(frame, len);
#endif

    if(serial_is_mcu_ack(frame, len)) {
        on_serial_mcu_ack(frame, len);
        return;
    }

    if(!serial_is_mcu_data(frame, len)) {
        return;
    }

    if(serial_ack_enabled() && !serial_ota_session_.is_active()) {
        send_soc_ack_for_bb(frame, len);
    }

    if(serial_ota_session_.on_serial_rx_frame(frame, len)) {
        return;
    }

    uint8_t door_state = 0;
    if(serial_parse_door_open_result(frame, len, &door_state) == 0) {
        const bool opened = (door_state == 0x00);
        {
            std::lock_guard<std::mutex> lock(door_open_rsp_mtx_);
            door_open_rsp_opened_  = opened;
            door_open_rsp_pending_ = true;
        }
        door_open_rsp_cv_.notify_all();
        UPC_LOG_MCU_LINK(Info, "door_open_result: state=0x%02X opened=%d",
                         static_cast<unsigned>(door_state), opened ? 1 : 0);
        return;
    }

    uint8_t clean_result = 0;
    if(serial_parse_clean_test_result(frame, len, &clean_result) == 0) {
        UPC_LOG_MCU_LINK(Info, "clean_test_done result=%u",
                         static_cast<unsigned>(clean_result));
        if(clean_test_done_cb_) {
            clean_test_done_cb_(clean_result);
        }
        return;
    }

    uint8_t transport_result = 0;
    if(serial_parse_transport_result(frame, len, &transport_result) == 0) {
        UPC_LOG_MCU_LINK(Info, "transport_result=%u",
                         static_cast<unsigned>(transport_result));
        if(transport_result_cb_) {
            transport_result_cb_(transport_result);
        }
        return;
    }

    /* 单条报警/EVENT 上报（0xA3 + 0x20 + TLV 0x01）：原样透传给 UI 消息中心 */
    serial_alarm_report_t alarm{};
    if(serial_parse_alarm_single_report(frame, len, &alarm) == 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Info,
            "alarm_report: code=%u sev=%u state=0x%02X clear=%u detail=%u",
            static_cast<unsigned>(alarm.alarm_code),
            static_cast<unsigned>(alarm.severity),
            static_cast<unsigned>(alarm.state_mask),
            static_cast<unsigned>(alarm.clear_mode),
            static_cast<unsigned>(alarm.detail));
        publish_alarm_report_ntf(alarm);
        SpiStatusLed::instance().on_alarm_report(alarm);
        return;
    }

    /* 报警快照的一个 slot（0xA3 + 0x81 + TLV 0x04）：应答 UI 的快照请求，
     * 用于重启/重连后补齐当前报警。稀疏响应，攒帧在 UI 侧。 */
    serial_alarm_snapshot_t snap{};
    if(serial_parse_alarm_snapshot(frame, len, &snap) == 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Info,
            "alarm_snapshot: slot %u (count=%u) active=0x%08X latched=0x%08X highest=%u",
            static_cast<unsigned>(snap.slot_id),
            static_cast<unsigned>(snap.nonzero_slot_count),
            static_cast<unsigned>(snap.active_mask),
            static_cast<unsigned>(snap.latched_mask),
            static_cast<unsigned>(snap.highest_severity));
        ingest_alarm_snapshot_slot(snap);
        return;
    }

    /* 报警复位失败回执（0xA3 + 0x82 + TLV 0x04）：与快照共用 TLV 号，靠 Cmd 区分，
     * 所以必须排在快照之后（快照那支已校验 Cmd==0x81，不会误吃）。 */
    serial_alarm_reset_result_t reset_res{};
    if(serial_parse_alarm_reset_result(frame, len, &reset_res) == 0) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn, "alarm_reset_result: code=%u result=%u (failed)",
                         static_cast<unsigned>(reset_res.alarm_code),
                         static_cast<unsigned>(reset_res.result));
        publish_alarm_reset_result_ntf(reset_res);
        return;
    }

    /* 工程版本上报（0xA2 + 0x10 + TLV 0x02）：About 页查询的应答 */
    serial_project_info_t pinfo{};
    if(serial_parse_project_info_report(frame, len, &pinfo) == 0) {
        UPC_LOG_MCU_LINK(Info,
            "mcu_version: %u.%u.%u laser=%u z_motor=%u slot=%u ota=%u",
            static_cast<unsigned>(pinfo.major),
            static_cast<unsigned>(pinfo.minor),
            static_cast<unsigned>(pinfo.patch),
            static_cast<unsigned>(pinfo.laser_type),
            static_cast<unsigned>(pinfo.z_motor_type),
            static_cast<unsigned>(pinfo.run_slot),
            static_cast<unsigned>(pinfo.ota_state));
        on_mcu_version_report(pinfo);
        return;
    }

    serial_sensor_report_t sensor{};
    serial_sensor_err_t serr = SERIAL_SENSOR_ERR_NONE;
    if(serial_parse_sensor_report_detail(frame, len, &sensor, &serr) == 0) {
        on_sensor_report(sensor);
        return;
    }

    /* V4 EXT（0x06）：须在「SENSOR FAIL」判定前识别，否则周期刷警告 */
    serial_sensor_ext_t sext{};
    if(serial_parse_sensor_ext_detail(frame, len, &sext, nullptr) == 0) {
        on_sensor_ext_report(sext);
        return;
    }

    /* 运动轴位置上报（0x9B + TLV 0x05）：Z/S/D 位置 + Z 工作坐标 */
    serial_motion_pos_t mpos{};
    if(serial_parse_motion_pos_report(frame, len, &mpos) == 0) {
        on_motion_pos_report(mpos);
        return;
    }

/* 激光器扩展上报（0x9B + TLV 0x04）：型号/状态/报警/功率/温度/电流（3ST；2ST 已由下位机停发，不再解析） */
LaserReport3 laser3{};
if(parse_laser_report3(frame, len, &laser3) == 0) {
    publish_laser_ntf(laser3.type, laser3.status, laser3.alarm,
                      laser3.power_raw, laser3.temp_raw, laser3.current_raw);
    UPC_LOG_MCU_LINK(Info,
        "laser(3ST): type=%u status=0x%04X alarm=0x%04X power=%u temp=%u cur=%u",
        laser3.type, laser3.status, laser3.alarm, laser3.power_raw,
        laser3.temp_raw, laser3.current_raw);
    return;
}

    const serial_event_t evt = serial_parse_frame(frame, len);
    if(evt != SERIAL_EVT_NONE) {
        notify_serial_event(evt);
    }
}

/* ---------- TCP 原始收发 ---------- */

/**
 * @brief 非阻塞 TCP 完整写：循环 send，短写续传；EAGAIN 时分片 poll POLLOUT。
 * @return 0 整帧写完；-1 未写出可整批重试；-2 半写/hangup，禁止整批重发
 */
int McuLink::tcp_send_raw(const uint8_t* data, size_t len)
{
    if(tcp_fd_ < 0 || !data || len == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(tcp_send_mtx_);
    tcp_log_tx_frame(data, len);

    constexpr int kPollSliceMs = 50;
    constexpr int kMaxWaitMs   = 2000;

    auto hangup = [this]() {
        if(tcp_fd_ >= 0) {
            ::close(tcp_fd_);
            tcp_fd_ = -1;
        }
        tcp_rx_accum_len_ = 0;
    };

    size_t off = 0;
    while(off < len) {
        if(tcp_fd_ < 0) {
            return off > 0 ? -2 : -1;
        }
        const ssize_t n = send(tcp_fd_, data + off, len - off, MSG_NOSIGNAL);
        if(n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if(n < 0 && errno == EINTR) {
            continue;
        }
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int waited = 0;
            bool ready = false;
            while(waited < kMaxWaitMs) {
                if(terminate_pending_.load()) {
                    if(off > 0) {
                        UPC_LOG_MCU_LINK(Warn,
                            "tcp_send_raw abort by terminate off=%zu/%zu",
                            off, len);
                        hangup();
                        return -2;
                    }
                    return -1;
                }
                struct pollfd pfd{};
                pfd.fd     = tcp_fd_;
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, kPollSliceMs);
                if(pr < 0) {
                    if(errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if(pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    hangup();
                    return off > 0 ? -2 : -1;
                }
                if(pr > 0 && (pfd.revents & POLLOUT)) {
                    ready = true;
                    break;
                }
                waited += kPollSliceMs;
            }
            if(ready) {
                continue;
            }
            UPC_LOG_MCU_LINK(Warn,
                "tcp_send_raw POLLOUT timeout off=%zu/%zu waited=%dms",
                off, len, kMaxWaitMs);
            if(off > 0) {
                UPC_LOG_MCU_LINK(Error,
                    "tcp_send_raw incomplete — hangup to resync");
                hangup();
                return -2;
            }
            return -1;
        }
        if(n < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN ||
                      errno == ECONNABORTED || errno == EBADF)) {
            hangup();
            return off > 0 ? -2 : -1;
        }
        UPC_LOG_MCU_LINK(Error, "tcp_send_raw fail errno=%d off=%zu/%zu",
                         errno, off, len);
        if(off > 0) {
            hangup();
            return -2;
        }
        return -1;
    }
    return 0;
}

int McuLink::tcp_recv_nonblock(uint8_t* buf, size_t buf_size)
{
    if(tcp_fd_ < 0 || !buf) {
        return -1;
    }
    const ssize_t n = recv(tcp_fd_, buf, buf_size, MSG_DONTWAIT);
    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // 暂无数据，属于正常情况
        }
        return -1; // 其他错误
    }
    if(n == 0) {
        // 对端优雅关闭
        close(tcp_fd_);
        tcp_fd_ = -1;
        return -1;
    }
    return (int)n;
}

void McuLink::tcp_drain_rx()
{
    uint8_t chunk[256];
    auto recv_fn = +[](uint8_t* b, size_t s) -> int {
        return McuLink::instance().tcp_recv_nonblock(b, s);
    };
    drain_framed_rx(recv_fn, chunk, sizeof(chunk),
                    tcp_rx_accum_, sizeof(tcp_rx_accum_), &tcp_rx_accum_len_,
                    tcp_guess_rx_frame_len,
                    +[](const uint8_t* f, size_t l) { tcp_handle_frame(f, l); });
}

/* ---------- 串口原始收发 ---------- */

int McuLink::serial_send_raw(const uint8_t* data, size_t len)
{
    if(serial_fd_ < 0 || !data || len == 0) {
        return -1;
    }
#if UPC_DEBUG && UPC_MOD_MCU_LINK
    serial_log_frame_hex("TX", data, len);
#endif
    /* 串口也为 O_NONBLOCK：短写续传，避免 TERMINATE/LAYER_DONE 半帧丢失 */
    constexpr int kPollSliceMs = 50;
    constexpr int kMaxWaitMs   = 1000;
    size_t off = 0;
    while(off < len) {
        if(serial_fd_ < 0) {
            return -1;
        }
        const ssize_t n = ::write(serial_fd_, data + off, len - off);
        if(n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if(n < 0 && errno == EINTR) {
            continue;
        }
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int waited = 0;
            bool ready = false;
            while(waited < kMaxWaitMs) {
                struct pollfd pfd{};
                pfd.fd     = serial_fd_;
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, kPollSliceMs);
                if(pr < 0) {
                    if(errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if(pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    return -1;
                }
                if(pr > 0 && (pfd.revents & POLLOUT)) {
                    ready = true;
                    break;
                }
                waited += kPollSliceMs;
            }
            if(ready) {
                continue;
            }
            UPC_LOG_MCU_LINK(Warn, "serial_send_raw POLLOUT timeout off=%zu/%zu",
                             off, len);
            return -1;
        }
        UPC_LOG_MCU_LINK(Warn, "serial_send_raw fail errno=%d off=%zu/%zu",
                         errno, off, len);
        return -1;
    }
    return 0;
}

void McuLink::serial_clear_reliable()
{
    serial_reliable_pending_ = {};
    serial_reliable_queue_.clear();
}

void McuLink::serial_start_reliable_item(const SerialReliableItem& item)
{
    if(item.len == 0 || item.len > sizeof(item.data)) {
        return;
    }

    serial_reliable_pending_.active = true;
    std::memcpy(serial_reliable_pending_.data, item.data, item.len);
    serial_reliable_pending_.len = item.len;
    serial_reliable_pending_.retry_count = 0;
    serial_reliable_pending_.abort_print_on_fail = item.abort_print_on_fail;

    serial_ack_key_t key{};
    if(serial_extract_ack_key(item.data, item.len, &key) != 0) {
        serial_reliable_pending_.active = false;
        UPC_LOG_MCU_LINK(Warn, "serial reliable: bad AA key");
        return;
    }
    serial_reliable_pending_.key = key;

    serial_send_raw(item.data, item.len);
    serial_reliable_pending_.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(GLAVO_SERIAL_ACK_TIMEOUT_MS);

    UPC_LOG_MCU_LINK(Debug,
        "serial reliable TX port=%02X cmd=%02X type=%02X abort=%d",
        key.port, key.cmd, key.type, item.abort_print_on_fail ? 1 : 0);
}

void McuLink::serial_advance_reliable_queue()
{
    serial_reliable_pending_.active = false;
    if(serial_reliable_queue_.empty()) {
        return;
    }
    const SerialReliableItem next = serial_reliable_queue_.front();
    serial_reliable_queue_.pop_front();
    serial_start_reliable_item(next);
}

void McuLink::serial_send_aa_reliable(const uint8_t* data, size_t len,
                                      bool abort_print_on_fail)
{
    if(!data || len == 0 || len > sizeof(SerialReliableItem::data)) {
        return;
    }

    if(!serial_ack_enabled()) {
        serial_send_raw(data, len);
        return;
    }

    if(!serial_is_soc_data(data, len)) {
        serial_send_raw(data, len);
        return;
    }

    SerialReliableItem item{};
    std::memcpy(item.data, data, len);
    item.len = len;
    item.abort_print_on_fail = abort_print_on_fail;

    if(serial_reliable_pending_.active) {
        serial_reliable_queue_.push_back(item);
        return;
    }
    serial_start_reliable_item(item);
}

void McuLink::on_serial_mcu_ack(const uint8_t* frame, size_t len)
{
    if(!serial_ack_enabled()) {
        return;
    }
    if(!serial_reliable_pending_.active) {
        return;
    }

    serial_ack_key_t key{};
    if(serial_extract_ack_key(frame, len, &key) != 0) {
        return;
    }
    if(!serial_ack_keys_match(&key, &serial_reliable_pending_.key)) {
        return;
    }

    UPC_LOG_MCU_LINK(Debug,
        "serial reliable ACK port=%02X cmd=%02X type=%02X",
        key.port, key.cmd, key.type);
    serial_advance_reliable_queue();
}

void McuLink::send_soc_ack_for_bb(const uint8_t* frame, size_t len)
{
    if(!serial_ack_enabled()) {
        return;
    }
    uint8_t ack[16];
    const int alen = serial_build_soc_ack(frame, len, ack, sizeof(ack));
    if(alen > 0) {
        serial_send_raw(ack, (size_t)alen);
    }
}

void McuLink::link_process_serial_reliable_tx()
{
    if(!serial_ack_enabled()) {
        return;
    }
    if(serial_ota_session_.is_active()) {
        return;
    }
    if(!serial_reliable_pending_.active) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if(now < serial_reliable_pending_.deadline) {
        return;
    }

    if(serial_reliable_pending_.retry_count >= GLAVO_SERIAL_ACK_MAX_RETRY) {
        UPC_LOG_MCU_LINK(Warn,
            "serial reliable timeout port=%02X cmd=%02X type=%02X retries=%d",
            serial_reliable_pending_.key.port,
            serial_reliable_pending_.key.cmd,
            serial_reliable_pending_.key.type,
            serial_reliable_pending_.retry_count);

        const bool abort = serial_reliable_pending_.abort_print_on_fail;
        serial_clear_reliable();

        if(abort) {
            PrintJobState::instance().set_job_error(true);
            parser::ParserManager::instance().stop_parse();
            request_terminate();
        }
        return;
    }

    ++serial_reliable_pending_.retry_count;
    serial_send_raw(serial_reliable_pending_.data,
                    serial_reliable_pending_.len);
    serial_reliable_pending_.deadline =
        now + std::chrono::milliseconds(GLAVO_SERIAL_ACK_TIMEOUT_MS);

    UPC_LOG_MCU_LINK(Warn,
        "serial reliable retry #%d port=%02X cmd=%02X type=%02X",
        serial_reliable_pending_.retry_count,
        serial_reliable_pending_.key.port,
        serial_reliable_pending_.key.cmd,
        serial_reliable_pending_.key.type);
}

int McuLink::serial_recv_nonblock(uint8_t* buf, size_t buf_size)
{
    if(serial_fd_ < 0 || !buf) {
        return -1;
    }
    const ssize_t n = read(serial_fd_, buf, buf_size);
    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // 暂无数据
        }
        return -1;
    }
    return (int)n;
}

void McuLink::serial_drain_rx()
{
    uint8_t chunk[64];
    auto recv_fn = +[](uint8_t* b, size_t s) -> int {
        return McuLink::instance().serial_recv_nonblock(b, s);
    };
    drain_framed_rx(recv_fn, chunk, sizeof(chunk),
                    serial_rx_accum_, sizeof(serial_rx_accum_), &serial_rx_accum_len_,
                    serial_guess_rx_frame_len,
                    +[](const uint8_t* f, size_t l) { serial_handle_frame(f, l); });
}

/* ---------- tcp_stage_ 队列操作 ---------- */

void McuLink::tcp_stage_clear()
{
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    tcp_stage_.clear();
}

size_t McuLink::tcp_stage_count()
{
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    return tcp_stage_.size();
}

bool McuLink::tcp_stage_peek_type(parser::rb_pkt_type_t* ptype)
{
    if(!ptype) {
        return false;
    }
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    if(tcp_stage_.empty()) {
        return false;
    }
    *ptype = static_cast<parser::rb_pkt_type_t>(tcp_stage_.front().type);
    return true;
}

bool McuLink::tcp_stage_pop_front(parser::rb_packet_t* pkt)
{
    if(!pkt) {
        return false;
    }
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    if(tcp_stage_.empty()) {
        return false;
    }
    *pkt = tcp_stage_.front();
    tcp_stage_.pop_front();
    return true;
}

/**
 * @brief 从 RingBuffer 预取包到 tcp_stage_
 *
 * 预取策略：
 * - 每次最多取 REFILL_STAGE_MAX_PACKETS 个包；
 * - 若 tcp_stage_ 已有 TCP_STAGE_MAX_PACKETS 个包则停止，避免内存无限增长；
 * - 对 RingBuffer 的 peek/get 操作限时 5ms，防止阻塞链路线程。
 */
void McuLink::refill_tcp_stage_from_ring()
{
    if(!rb_) {
        return;
    }

    size_t moved = 0;
    while(moved < REFILL_STAGE_MAX_PACKETS) {
        {
            std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
            if(tcp_stage_.size() >= TCP_STAGE_MAX_PACKETS) {
                break; // 暂存队列已满
            }
        }

        parser::rb_pkt_type_t ptype;
        if(!rb_->peek_type(ptype)) {
            break; // RingBuffer 已空
        }

        parser::rb_packet_t pkt;
        if(!rb_->get(pkt, 5)) {
            break; // 获取失败（超时或无数据）
        }

        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        tcp_stage_.push_back(pkt);
        moved++;
    }
}

/* ---------- 状态辅助 ---------- */

bool McuLink::mcu_rates_drained(int pending_packets,
                                std::chrono::steady_clock::time_point since) const
{
    const auto snap = PrintJobState::instance().snapshot();
    if(snap.cmd_cache_rate  > static_cast<uint16_t>(MCU_DRAIN_MAX_RATE) ||
       snap.data_cache_rate > static_cast<uint16_t>(MCU_DRAIN_MAX_RATE)) {
        return false;
    }
    if(pending_packets <= 0) {
        return true; // 未下发过运动包，无需确认缓存
    }
    /* 已下发运动包后，须先观测到缓存非空再接受归零，避免短 G-code 心跳未刷新时误判 */
    if(mcu_rates_seen_nonempty_.load()) {
        return true;
    }
    /* 从未观测到非空：可能是超薄层/超小文件在两帧心跳之间就被 MCU 执行完。
     * 按已等待时长兜底放行，包数不足一个 batch 时用更短的窗口。 */
    if(since.time_since_epoch().count() == 0) {
        return false; // 尚未开始计时，等下一轮
    }
    const int64_t need_ms = (pending_packets < current_tcp_batch_size())
        ? static_cast<int64_t>(MCU_DRAIN_SMALL_BATCH_MS)
        : static_cast<int64_t>(MCU_DRAIN_FALLBACK_MS);
    if(need_ms <= 0) {
        return false; // 兜底已禁用，维持原“必须观测到非空”语义
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - since).count();
    return waited >= need_ms;
}

bool McuLink::motion_stream_active() const
{
    /* natural_complete_pending 阶段须发心跳刷新 rate，不能视为活跃下发 */
    return is_sending_.load() && !waiting_layer_.load() &&
           !terminate_pending_.load() && !natural_complete_pending_.load();
}

bool McuLink::motion_stream_backpressured() const
{
    if(!motion_stream_active()) {
        return false;
    }
    const auto snap = PrintJobState::instance().snapshot();
    const uint32_t threshold =
        (65535u * current_cmd_backpressure_pct()) / 100u;
    return snap.cmd_cache_rate >= threshold;
}

void McuLink::poll_mcu_drain_heartbeat()
{
    if(tcp_fd_ < 0) {
        return;
    }
    uint8_t hb[64];
    const int hblen = build_heartbeat_frame(hb, sizeof(hb));
    if(hblen > 0) {
        tcp_send_raw(hb, (size_t)hblen);
        tcp_drain_rx();
    }
}

/* ---------- 开始阈值判断 ---------- */

/**
 * @brief 判断 RingBuffer 是否满足开始发送的阈值条件
 * @return true 表示可以立即启动发送
 *
 * 判定逻辑：
 * 1. 若 RingBuffer 中已缓存 >= 当前生效 batch 包数，直接满足；
 * 2. 若解析器仍在解析中：
 *    - 若总运动包数 >= batch 且当前有包，则满足；
 *    - 若处于 start_pending 挂起状态，且文件已读完、总包数 < batch 但 > 0，
 *      说明文件很小，也满足；
 * 3. 若解析器已结束解析但文件已打开：只要总包数 > 0 且有包，即满足。
 */
bool McuLink::ring_meets_start_threshold() const
{
    if(!rb_) {
        return false;
    }

    parser::IParserWorker* parser = active_parser();
    if(!parser) {
        return false;
    }

    const size_t count = rb_->count();
    const int batch = current_tcp_batch_size();
    const int total = parser->get_total_motion_packets();

    if(count >= static_cast<size_t>(batch)) {
        return true;
    }
    if(parser->is_parsing()) {
        if(total >= batch && count > 0) {
            return true;
        }
        if(start_pending_.load() && parser->is_file_read_exhausted() &&
           total > 0 && total < batch && count > 0) {
            return true; // 小文件特例
        }
        return false;
    }
    if(parser->is_file_open()) {
        return total > 0 && count > 0;
    }
    return false;
}

/* ---------- 运动状态管理 ---------- */

void McuLink::reset_motion_state(bool clear_start_pending)
{
    if(clear_start_pending) {
        start_pending_.store(false);
    }
    is_sending_.store(false);
    user_paused_.store(false);
    waiting_layer_.store(false);
    layer_drain_pending_.store(false);
    layer_drain_since_ = {};
    natural_complete_pending_.store(false);
    natural_complete_since_ = {};
    mcu_rates_seen_nonempty_.store(false);
    serial_handshake_phase_.store(SERIAL_HS_NONE);
    serial_evt_condition_ok_.store(false);
    serial_evt_print_ready_.store(false);
    o2_hold_.store(false);
    /* 兜底：终止/完成路径已关阀；其它 reset 入口也勿让洗气阀挂着 */
    close_print_gas_wash_if_opened();
    if(o2_alarm_active_) {
        publish_soc_chamber_oxygen_alarm(false);
    }
    sent_at_layer_ready_ = 0;
    current_layer_snapped_ = false;
    first_layer_check_passed_ = false;
    laser_power_log_valid_ = false;
    tcp_stage_clear();
    serial_clear_reliable();
    PrintJobState::instance().set_sending(false);
}

void McuLink::begin_sending_internal()
{
    powder_low_latched_ = false;
    user_paused_.store(false);
    o2_hold_.store(false);
    if(o2_alarm_active_) {
        publish_soc_chamber_oxygen_alarm(false);
    }
    last_o2_wait_log_ = {};
    gas_wash_opened_by_soc_.store(false);
    is_sending_.store(true);
    waiting_layer_.store(false);
    layer_drain_pending_.store(false);
    layer_drain_since_ = {};
    natural_complete_pending_.store(false);
    natural_complete_since_ = {};
    mcu_rates_seen_nonempty_.store(false);
    tcp_stage_clear();

    // 记录首层高度，用于串口首次握手（后续换层更新为下层高度）
    parser::IParserWorker* parser = active_parser();
    active_layer_height_um_ =
        parser ? parser->get_layer_height_um() : GCODE_DEFAULT_LAYER_UM;
    current_layer_snapped_  = false;
    first_layer_check_passed_ = false;

    // 重置 PrintJobState 中的发送相关计数与标志
    PrintJobState::instance().set_sending(true);
    PrintJobState::instance().set_job_completed(false);
    PrintJobState::instance().set_job_error(false);
    PrintJobState::instance().set_sent_packets(0);

#if GLAVO_ENABLE_LAYER_CHANGE
    // 若串口已打开：打印请求 → CONDITION_OK → PRINT_READY
    // 测试图无 M3000：跳过首层串口握手，直接下发运动包
    if(!test_image_motion_.load() && serial_fd_ >= 0) {
        waiting_layer_.store(true);
        serial_evt_condition_ok_.store(false);
        serial_evt_print_ready_.store(false);
        uint8_t sbuf[32];
        const int slen = serial_build_print_request(sbuf, sizeof(sbuf));
        if(slen > 0) {
            UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "serial TX cmd: PRINT_REQUEST");
            serial_send_aa_reliable(sbuf, static_cast<size_t>(slen), true);
        }
        serial_handshake_phase_.store(SERIAL_HS_WAIT_CONDITION_OK);
    }
#endif

    UPC_LOG_MCU_LINK(Info, "begin_sending test_image=%d",
                     test_image_motion_.load() ? 1 : 0);
}

void McuLink::set_test_image_motion(bool on) noexcept
{
    test_image_motion_.store(on);
}

bool McuLink::is_test_image_motion() const noexcept
{
    return test_image_motion_.load();
}

void McuLink::begin_sending(bool skip_layer_handshake)
{
    if(tcp_fd_ < 0) {
        return; // TCP 未连接，无法开始
    }
    if(is_sending_.load()) {
        return; // 已在发送中
    }
    /* terminate/cleanup 未完成时禁止起印，否则会与 run_manual_terminate_cleanup 竞态 */
    if(terminate_pending_.load() || completing_cleanup_.load()) {
        UPC_LOG_MCU_LINK(Warn,
            "begin_sending refused: terminate/cleanup in progress");
        return;
    }
    cancel_pending_red_reassert();
    if(skip_layer_handshake) {
        test_image_motion_.store(true);
    } else {
        test_image_motion_.store(false);
    }
    if(!ring_meets_start_threshold()) {
        // 缓存不足，设置挂起标志，由链路线程在条件满足时自动启动
        start_pending_.store(true);
        return;
    }
    start_pending_.store(false);
    begin_sending_internal();
}

void McuLink::try_complete_pending_start()
{
    if(!start_pending_.load() || is_sending_.load() || tcp_fd_ < 0) {
        return;
    }
    if(terminate_pending_.load() || completing_cleanup_.load()) {
        return;
    }
    if(!ring_meets_start_threshold()) {
        return; // 仍不满足阈值，继续等待
    }
    start_pending_.store(false);
    begin_sending_internal();
}

/* ---------- 终止请求 ---------- */

void McuLink::request_terminate()
{
    user_paused_.store(false);
    terminate_pending_.store(true);
    is_sending_.store(false);
    start_pending_.store(false);
    PrintJobState::instance().set_sending(false);
}

void McuLink::set_user_dispatch_paused(bool paused)
{
    user_paused_.store(paused);
    UPC_LOG_MCU_LINK(Info, "user dispatch paused=%d", paused ? 1 : 0);
}

bool McuLink::is_user_dispatch_paused() const noexcept
{
    return user_paused_.load();
}

bool McuLink::is_o2_hold() const noexcept
{
    return o2_hold_.load();
}

bool McuLink::send_tcp_emergency_ctrl_ex(uint8_t stop_code, uint8_t pause_code, uint8_t red_code)
{
    uint8_t frame[64];
    const int flen = tcp_build_emergency_frame_ex(stop_code, pause_code, red_code, frame, sizeof(frame));
    if(flen <= 0) {
        UPC_LOG_MCU_LINK_TO(TcpMotionTx, Warn,
            "build tcp emergency frame failed stop=%u pause=%u red=%u",
            static_cast<unsigned>(stop_code),
            static_cast<unsigned>(pause_code),
            static_cast<unsigned>(red_code));
        return false;
    }
    const int rc = tcp_send_raw(frame, static_cast<size_t>(flen));
    if(rc != 0) {
        UPC_LOG_MCU_LINK_TO(TcpMotionTx, Warn,
            "tcp emergency send failed rc=%d stop=%u pause=%u red=%u",
            rc,
            static_cast<unsigned>(stop_code),
            static_cast<unsigned>(pause_code),
            static_cast<unsigned>(red_code));
        return false;
    }
    if(red_code == 1u || red_code == 2u || red_code == 3u) {
        last_requested_red_code_.store(red_code);
        if(red_code == 2u) {
            /* 已切激光：作废排队里的红光补发，避免盖掉 Red=2。 */
            reassert_red_after_laser_off_.store(false);
        }
    }
    UPC_LOG_MCU_LINK_TO(TcpMotionTx, Info, "tcp emergency sent stop=%u pause=%u red=%u",
                     static_cast<unsigned>(stop_code),
                     static_cast<unsigned>(pause_code),
                     static_cast<unsigned>(red_code));
    return true;
}

float McuLink::get_z_pos_mm() const noexcept
{
    return z_pos_mm_cache_.load(std::memory_order_relaxed);
}

bool McuLink::send_debug_motion_subpacket(const uint8_t subpacket[11])
{
    if(!subpacket || is_sending_.load() || start_pending_.load() ||
       terminate_pending_.load()) {
        UPC_LOG_MCU_LINK(Warn,
            "debug motion rejected sending=%d start_pending=%d terminate=%d",
            is_sending_.load() ? 1 : 0,
            start_pending_.load() ? 1 : 0,
            terminate_pending_.load() ? 1 : 0);
        return false;
    }

    uint8_t frame[14];
    const int flen = tcp_build_motion_frame(subpacket, 1, frame, sizeof(frame));
    if(flen <= 0) {
        UPC_LOG_MCU_LINK(Warn, "build debug motion frame failed");
        return false;
    }
    const int rc = tcp_send_raw(frame, static_cast<size_t>(flen));
    if(rc != 0) {
        UPC_LOG_MCU_LINK(Warn, "debug motion send failed rc=%d", rc);
        return false;
    }
    return true;
}

bool McuLink::send_galvo_abs_pos(float x_mm, float y_mm,
                                 float speed_mm_min, int laser_on)
{
    /* 1. 互斥：打印进行中拒绝手动移动，避免与批量运动包冲突 */
    if(is_sending()) {
        UPC_LOG_MCU_LINK(Warn,
            "send_galvo_abs_pos rejected: print in progress (is_sending)");
        return false;
    }
    /* 2. TCP 必须已连接 */
    if(tcp_fd_ < 0) {
        UPC_LOG_MCU_LINK(Warn, "send_galvo_abs_pos failed: tcp not connected");
        return false;
    }

    /* 3. 从 ParserContext 取校准数据与增益（与切片下发同源） */
    const parser::ParserContext& ctx = parser::ParserContext::instance();
    const parser::CalData& cal = ctx.cal_data();
    const float scale_gain = ctx.scale_gain();
    float range_gain = ctx.range_gain();
    const float speed_gain = ctx.speed_gain();

    /* 4. 校准变换：切片坐标 → 物理坐标（中心平移 + 网格双线性畸变） */
    float out_x = 0.0f, out_y = 0.0f;
    parser::cal_apply_xy(&cal, x_mm, y_mm, &out_x, &out_y);
    const float ox = out_x * scale_gain;
    const float oy = out_y * scale_gain;

    /* 5. 构造 11B 运动包（与 OvfWorker::build_and_push_packet 编码完全一致） */
    uint8_t pkt[11];
    pkt[0] = 0x01;  // 运动包标识
    uint8_t lo = 0, hi = 0;
    parser::coordinate_to_offset_bytes(ox, range_gain, &lo, &hi);
    pkt[1] = lo;  // X 低字节
    pkt[2] = hi;  // X 高字节
    parser::coordinate_to_offset_bytes(oy, range_gain, &lo, &hi);
    pkt[3] = lo;  // Y 低字节
    pkt[4] = hi;  // Y 高字节
    pkt[5] = 0;   // 保留
    pkt[6] = 0;
    const float eff_speed = (speed_mm_min > 0.0f) ? speed_mm_min : 1000.0f;
    const int speed_int = static_cast<int>(eff_speed * speed_gain);
    parser::int_to_u16_bytes(speed_int, &lo, &hi);
    pkt[7] = lo;   // 速度低字节
    pkt[8] = hi;   // 速度高字节
    parser::int_to_u16_bytes(laser_on ? 1 : 0, &lo, &hi);
    pkt[9]  = lo;  // 激光开关低字节
    pkt[10] = hi;  // 激光开关高字节

    /* 6. 组帧并发送（1 个子包） */
    uint8_t frame[3 + 11];
    const int flen = build_motion_frame(pkt, 1, frame, sizeof(frame));
    if(flen <= 0) {
        UPC_LOG_MCU_LINK(Error, "send_galvo_abs_pos: build_motion_frame failed");
        return false;
    }
    const int rc = tcp_send_raw(frame, static_cast<size_t>(flen));
    if(rc != 0) {
        UPC_LOG_MCU_LINK(Warn,
            "send_galvo_abs_pos tcp send failed rc=%d x=%.2f y=%.2f",
            rc, static_cast<double>(x_mm), static_cast<double>(y_mm));
        return false;
    }
    UPC_LOG_MCU_LINK(Info,
        "galvo abs pos sent: x=%.2f y=%.2f speed=%.0f laser=%d "
        "ox=%.3f oy=%.3f range=%.2f scale=%.3f",
        static_cast<double>(x_mm), static_cast<double>(y_mm),
        static_cast<double>(eff_speed), laser_on,
        static_cast<double>(ox), static_cast<double>(oy),
        static_cast<double>(range_gain), static_cast<double>(scale_gain));
    return true;
}

bool McuLink::send_tcp_emergency_ctrl(bool stop, bool pause)
{
    const uint8_t stop_code = stop ? 1u : 0u;
    const uint8_t pause_code = pause ? 2u : 1u;
    return send_tcp_emergency_ctrl_ex(stop_code, pause_code, 0u);
}

void McuLink::force_switch_to_red_mode_on_tcp_connect()
{
    /* 振镜板与串口 MCU 独立：连上只切 TCP 红光。关激光使能等真实版本上报。 */
    if(!send_tcp_emergency_ctrl_ex(0u, 0u, 1u)) {
        UPC_LOG_MCU_LINK(Warn, "tcp connect failsafe: switch to red mode failed");
        return;
    }
    UPC_LOG_MCU_LINK(Info, "tcp connect failsafe: switched to red mode");
}

void McuLink::apply_serial_laser_off_after_mcu_ready()
{
    if(serial_laser_off_applied_.load()) {
        return;
    }
    if(serial_fd_ < 0 || serial_ota_session_.is_active()) {
        return;
    }
    /* 打印中关使能会掐出光；记 pending，空闲后再发。 */
    if(is_sending_.load() || print_task_flow_active_.load()) {
        if(!serial_laser_off_pending_.exchange(true)) {
            UPC_LOG_MCU_LINK(Info,
                "mcu version failsafe: serial laser-off deferred (print active)");
        }
        return;
    }

    uint8_t sbuf[SERIAL_DEBUG_MAX_FRAME];
    const int slen = serial_debug_build_switch(0u, false, sbuf, sizeof(sbuf));
    if(slen <= 0) {
        UPC_LOG_MCU_LINK(Warn, "mcu version failsafe: build serial laser-off failed");
        return;
    }
    enqueue_serial_tx(sbuf, static_cast<size_t>(slen), false);
    serial_laser_off_applied_.store(true);
    serial_laser_off_pending_.store(false);
    request_reassert_red_after_serial_tx(1u);
    UPC_LOG_MCU_LINK(Info, "mcu version failsafe: serial laser-off queued");
}

void McuLink::maybe_reassert_red_after_serial_laser_off()
{
    if(!reassert_red_after_laser_off_.load()) {
        return;
    }
    /* 串口还没发出 0x12：下一轮再试，不要先把标志清掉。 */
    if(serial_fd_ < 0 || serial_ota_session_.is_active()) {
        return;
    }
    if(is_sending_.load()) {
        reassert_red_after_laser_off_.store(false);
        UPC_LOG_MCU_LINK(Info, "skip red reassert: motion sending");
        return;
    }
    const uint8_t red_code = reassert_red_code_.load();
    const uint8_t want = last_requested_red_code_.load();
    if(want == 2u || want != red_code) {
        reassert_red_after_laser_off_.store(false);
        UPC_LOG_MCU_LINK(Info,
            "skip red reassert: stale want=%u pending=%u",
            static_cast<unsigned>(want),
            static_cast<unsigned>(red_code));
        return;
    }
    if(!reassert_red_after_laser_off_.exchange(false)) {
        return;
    }
    if(tcp_fd_ < 0) {
        return;
    }
    if(!send_tcp_emergency_ctrl_ex(0u, 0u, red_code)) {
        UPC_LOG_MCU_LINK(Warn, "reassert red failed code=%u",
                         static_cast<unsigned>(red_code));
        return;
    }
    UPC_LOG_MCU_LINK(Info, "red after laser-off code=%u",
                     static_cast<unsigned>(red_code));
}

void McuLink::set_print_task_flow_active(bool active) noexcept
{
    print_task_flow_active_.store(active);
}

bool McuLink::is_print_task_flow_active() const noexcept
{
    return print_task_flow_active_.load();
}

/* ---------- 清理辅助：协议帧发送 ---------- */

void McuLink::send_tcp_reset()
{
    (void)send_tcp_emergency_ctrl_ex(1u, 0u, 0u);
}

void McuLink::send_serial_layer_done(uint16_t next_layer_um)
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ < 0) {
        return;
    }
    uint8_t sbuf[32];
    const int slen = serial_build_layer_done(next_layer_um, sbuf, sizeof(sbuf));
    if(slen > 0) {
        UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "serial TX cmd: LAYER_DONE (next_layer=%u um)",
            static_cast<unsigned>(next_layer_um));
        serial_send_aa_reliable(sbuf, static_cast<size_t>(slen), true);
    }
#else
    (void)next_layer_um;
#endif
}

void McuLink::send_serial_print_terminate()
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_terminate(sbuf, sizeof(sbuf));
        if(slen > 0) {
            UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "serial TX cmd: PRINT_TERMINATE");
            serial_send_aa_reliable(sbuf, (size_t)slen, false);
        }
    }
#endif
}

void McuLink::send_serial_print_finished()
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_finished(sbuf, sizeof(sbuf));
        if(slen > 0) {
            UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "serial TX cmd: PRINT_FINISHED");
            serial_send_aa_reliable(sbuf, (size_t)slen, false);
        }
    }
#endif
}

void McuLink::send_serial_gas_wash(bool on)
{
    if(serial_fd_ < 0) {
        return;
    }
    uint8_t sbuf[SERIAL_DEBUG_MAX_FRAME];
    const int slen = serial_debug_build_switch(1u, on, sbuf, sizeof(sbuf));
    if(slen <= 0) {
        UPC_LOG_MCU_LINK(Warn, "serial gas wash build failed on=%d", on ? 1 : 0);
        return;
    }
    UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "serial TX cmd: GAS_WASH_%s",
                        on ? "ON" : "OFF");
    /* 开阀走可靠发送，避免洗气未开就 PRINT_START；关阀尽力发出即可 */
    if(on) {
        serial_send_aa_reliable(sbuf, static_cast<size_t>(slen), true);
    } else {
        serial_send_aa_reliable(sbuf, static_cast<size_t>(slen), false);
    }
}

void McuLink::ensure_print_gas_wash_on()
{
    if(gas_wash_opened_by_soc_.load()) {
        return;
    }
    send_serial_gas_wash(true);
    gas_wash_opened_by_soc_.store(true);
}

void McuLink::close_print_gas_wash_if_opened()
{
    if(!gas_wash_opened_by_soc_.exchange(false)) {
        return;
    }
    send_serial_gas_wash(false);
}

/* ---------- 终止/完成清理流程 ---------- */

bool McuLink::run_manual_terminate_cleanup()
{
    bool expected = false;
    if(!completing_cleanup_.compare_exchange_strong(expected, true)) {
        /* 已有 cleanup（自然完成/上一次终止）在跑：本轮什么都没做，
         * 由调用方保留 terminate_pending_ 下一轮重试，不能当成已完成。 */
        UPC_LOG_MCU_LINK(Warn,
            "manual terminate deferred: cleanup already in progress");
        return false;
    }

    // 1. 向振镜发 0x0A(Stop=1) 终止指令，向 MCU 发终止通知
    send_tcp_reset();
    send_serial_print_terminate();
    close_print_gas_wash_if_opened();

    // 2. 停止解析、清空 RingBuffer 与 stage 队列
    parser::ParserManager::instance().stop_parse();
    if(rb_) {
        rb_->clear();
    }
    tcp_stage_clear();

    // 3. 判断是否是错误导致的终止，选择对应事件上报
    const bool was_error = PrintJobState::instance().snapshot().job_error;
    reset_motion_state(true);
    PrintJobState::instance().reset_job_counters();

    emit_event(was_error ? McuLinkEvent::PrintError : McuLinkEvent::TerminateDone);

    completing_cleanup_.store(false);
    return true;
}

void McuLink::link_try_final_layer_snap()
{
#if !GLAVO_ENABLE_LAYER_SNAP
    return;
#endif
    if(current_layer_snapped_) {
        return;
    }
    if(PrintJobState::instance().snapshot().sent_packets <= sent_at_layer_ready_) {
        return;
    }
    PrintVideoNotify::instance().on_layer_snap(active_layer_height_um_);
    current_layer_snapped_ = true;
}

void McuLink::run_natural_complete_cleanup()
{
    bool expected = false;
    if(!completing_cleanup_.compare_exchange_strong(expected, true)) {
        return;
    }

    const bool test_motion = test_image_motion_.load();
    // 1. 正式打印：末层无 M3000 时补 Snap，再通知 MCU 打印完成
    //    测试图：无换层、无录像，跳过 Snap / PRINT_FINISHED
    if(!test_motion) {
        link_try_final_layer_snap();
        send_serial_print_finished();
    }
    close_print_gas_wash_if_opened();

    // 2. 先清发送态，再发 TCP 0x0A(Stop=1)（避免 is_sending 时断线误触发 terminate）
    reset_motion_state(true);
    PrintJobState::instance().set_job_completed(true);
    PrintJobState::instance().set_sending(false);
    send_tcp_reset();

    emit_event(McuLinkEvent::PrintCompleted);

    completing_cleanup_.store(false);
}

/* ---------- 串口握手与换层处理 ---------- */

void McuLink::link_process_serial_handshake()
{
#if !GLAVO_ENABLE_LAYER_CHANGE
    return; // 换层功能未启用，无需串口握手
#endif
    if(!waiting_layer_.load() || serial_fd_ < 0) {
        return; // 不处于等待状态或串口未打开
    }

    const int phase = serial_handshake_phase_.load();

    // 阶段一：MCU PRINT_CONDITION_OK 后
    //   - 氧门开：SOC 发洗气开，等氧 < 阈值再 PRINT_START
    //   - Skip O2 / EnvSim / 测试图：不发洗气开，直接 PRINT_START
    if(phase == SERIAL_HS_WAIT_CONDITION_OK &&
       serial_evt_condition_ok_.load()) {
        if(soc_o2_print_gate_enabled()) {
            ensure_print_gas_wash_on();
            float o2 = -1.f;
            const bool o2_ok = read_print_o2_percent(&o2) &&
                               o2 < GLAVO_O2_PRINT_MAX_PERCENT;
            if(!o2_ok) {
                /* 起印尚未开扫，不做 TCP 暂停；仍发 1120 让 UI 提示氧高。 */
                publish_soc_chamber_oxygen_alarm(true);
                const auto now = std::chrono::steady_clock::now();
                if(last_o2_wait_log_.time_since_epoch().count() == 0 ||
                   now - last_o2_wait_log_ >= std::chrono::seconds(5)) {
                    last_o2_wait_log_ = now;
                    UPC_LOG_MCU_LINK_TO(SerialLayer, Info,
                        "gas wash on; wait SOC O2 < %.2f%% before PRINT_START (now=%s%.2f%%)",
                        static_cast<double>(GLAVO_O2_PRINT_MAX_PERCENT),
                        (o2 >= 0.f) ? "" : "invalid/",
                        static_cast<double>((o2 >= 0.f) ? o2 : -1.f));
                }
                return;
            }
            last_o2_wait_log_ = {};
            publish_soc_chamber_oxygen_alarm(false);
            UPC_LOG_MCU_LINK_TO(SerialLayer, Info,
                "SOC O2 gate pass: %.2f%% < %.2f%%, send PRINT_START",
                static_cast<double>(o2),
                static_cast<double>(GLAVO_O2_PRINT_MAX_PERCENT));
        } else {
            UPC_LOG_MCU_LINK_TO(SerialLayer, Info,
                "O2 gate skipped (skip=%d env_sim=%d test=%d): no gas wash, PRINT_START",
                skip_o2_print_gate_.load() ? 1 : 0,
                env_sim_on_.load() ? 1 : 0,
                test_image_motion_.load() ? 1 : 0);
        }
        serial_evt_condition_ok_.store(false);
        uint8_t sbuf[32];
        const int slen = serial_build_print_start(active_layer_height_um_, sbuf, sizeof(sbuf));
        if(slen > 0) {
            UPC_LOG_MCU_LINK_TO(SerialLayer, Info, "serial TX cmd: PRINT_START (layer=%u um)",
                static_cast<unsigned>(active_layer_height_um_));
            serial_send_aa_reliable(sbuf, (size_t)slen, true);
        }
        serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
        serial_evt_print_ready_.store(false);
        return;
    }

    // 阶段二：收到 PRINT_READY 后，解除等待，开始下发本层运动包
    if(phase == SERIAL_HS_WAIT_PRINT_READY &&
       serial_evt_print_ready_.exchange(false)) {
        if(!first_layer_check_passed_ && first_layer_check_cb_) {
            UPC_LOG_MCU_LINK(Info, "first layer visual check request layer=%u",
                static_cast<unsigned>(active_layer_height_um_));
            const bool pass = first_layer_check_cb_(active_layer_height_um_);
            if(!pass) {
                if(terminate_pending_.load()) {
                    UPC_LOG_MCU_LINK(Info,
                        "first layer visual check aborted by terminate");
                    return;
                }
                /* 首层检测失败：不上光，通知 MCU 继续铺粉（层高=0，不降基板）。 */
                UPC_LOG_MCU_LINK(Warn, "first layer visual check failed, request recoat");
                send_serial_layer_done(0);
                serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
                serial_evt_print_ready_.store(false);
                return;
            }
            first_layer_check_passed_ = true;
            UPC_LOG_MCU_LINK(Info, "first layer visual check passed");
        }
        sent_at_layer_ready_ = PrintJobState::instance().snapshot().sent_packets;
        current_layer_snapped_ = false;
        waiting_layer_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
    }
}

bool McuLink::soc_o2_print_gate_enabled() const noexcept
{
    if(test_image_motion_.load() || env_sim_on_.load() ||
        skip_o2_print_gate_.load()) {
        return false;
    }
    return true;
}

bool McuLink::read_print_o2_percent(float* oxygen_percent) const
{
    if(!oxygen_percent) {
        return false;
    }
    if(env_sim_on_.load()) {
        std::lock_guard<std::mutex> lock(env_mtx_);
        if(env_cache_.oxygen_percent < 0.f) {
            return false;
        }
        *oxygen_percent = env_cache_.oxygen_percent;
        return true;
    }
    const SocEnvSample soc = SocSensors::instance().snapshot();
    if(soc.oxygen_percent < 0.f) {
        return false;
    }
    *oxygen_percent = soc.oxygen_percent;
    return true;
}

void McuLink::publish_soc_chamber_oxygen_alarm(bool active)
{
    if(o2_alarm_active_ == active) {
        return;
    }
    o2_alarm_active_ = active;

    serial_alarm_report_t report{};
    report.alarm_code = static_cast<uint16_t>(GLAVO_ALARM_CODE_CHAMBER_OXYGEN_HIGH);
    report.severity = 2; /* warning，与字典 CHAMBER_OXYGEN_HIGH 一致 */
    report.state_mask = active ? SERIAL_ALARM_STATE_ACTIVE : 0;
    report.clear_mode = SERIAL_ALARM_CLEAR_AUTO;
    report.detail = 0;

    /* UPC_LOG_* 的 lvl 必须是 Level 枚举名，不能写三元表达式。 */
    if(active) {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Warn,
            "SOC chamber oxygen alarm ACTIVE (code=%u)",
            static_cast<unsigned>(GLAVO_ALARM_CODE_CHAMBER_OXYGEN_HIGH));
    } else {
        UPC_LOG_MCU_LINK_TO(SerialAlarm, Info,
            "SOC chamber oxygen alarm NORMAL (code=%u)",
            static_cast<unsigned>(GLAVO_ALARM_CODE_CHAMBER_OXYGEN_HIGH));
    }
    publish_alarm_report_ntf(report);
    SpiStatusLed::instance().on_alarm_report(report);
}

void McuLink::link_process_o2_print_gate()
{
    if(!soc_o2_print_gate_enabled()) {
        if(o2_hold_.load() || o2_alarm_active_) {
            o2_hold_.store(false);
            publish_soc_chamber_oxygen_alarm(false);
            if(!user_paused_.load()) {
                (void)send_tcp_emergency_ctrl(false, false);
            }
            emit_event(McuLinkEvent::OxygenRecovered);
        }
        return;
    }
    if(!is_sending_.load() || terminate_pending_.load()) {
        return;
    }
    /* 起印等 CONDITION/氧/PRINT_READY 阶段尚未开扫，只做门闸不在此暂停。 */
    if(waiting_layer_.load() &&
       serial_handshake_phase_.load() != SERIAL_HS_NONE) {
        return;
    }

    float o2 = -1.f;
    const bool valid = read_print_o2_percent(&o2);
    const bool too_high = !valid || o2 >= GLAVO_O2_PRINT_MAX_PERCENT;
    const bool recovered = valid && o2 <= GLAVO_O2_PRINT_RESUME_PERCENT;

    if(too_high) {
        if(!o2_hold_.load()) {
            o2_hold_.store(true);
            publish_soc_chamber_oxygen_alarm(true);
            if(!send_tcp_emergency_ctrl(false, true)) {
                UPC_LOG_MCU_LINK(Warn, "O2 hold: tcp pause send failed");
            }
            UPC_LOG_MCU_LINK(Warn,
                "O2 hold: pause print (o2=%s%.2f%% threshold=%.2f%%)",
                valid ? "" : "invalid/",
                static_cast<double>(valid ? o2 : -1.f),
                static_cast<double>(GLAVO_O2_PRINT_MAX_PERCENT));
            emit_event(McuLinkEvent::OxygenHigh);
        } else if(!o2_alarm_active_) {
            publish_soc_chamber_oxygen_alarm(true);
        }
        return;
    }

    if(o2_hold_.load() && recovered) {
        o2_hold_.store(false);
        publish_soc_chamber_oxygen_alarm(false);
        if(!user_paused_.load()) {
            if(!send_tcp_emergency_ctrl(false, false)) {
                UPC_LOG_MCU_LINK(Warn, "O2 recover: tcp resume send failed");
            }
        }
        UPC_LOG_MCU_LINK(Info,
            "O2 hold clear: o2=%.2f%% <= %.2f%% user_paused=%d",
            static_cast<double>(o2),
            static_cast<double>(GLAVO_O2_PRINT_RESUME_PERCENT),
            user_paused_.load() ? 1 : 0);
        emit_event(McuLinkEvent::OxygenRecovered);
    }
}

bool McuLink::link_handle_layer_mark()
{
    parser::rb_pkt_type_t ptype;
    if(!tcp_stage_peek_type(&ptype) || ptype != parser::RB_PKT_LAYER_MARK) {
        return false; // 队列头部不是换层标记
    }

    parser::rb_packet_t mark;
    if(!tcp_stage_pop_front(&mark)) {
        return false;
    }

#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ < 0) {
        UPC_LOG_MCU_LINK(Warn, "layer mark skipped: serial not open");
        return false; // 无串口无法握手，已弹出 mark 以免堵死运动队列
    }
    // 读取换层标记中的下层成高（协议 0x05 载荷）
    const uint16_t mark_um = parser::rb_layer_mark_get_height_um(mark);
    if(mark_um > 0) {
        active_layer_height_um_ = mark_um;
    }
    // 本层尚未下发任何扫描指令时跳过层标记（如文件开头多余的 M3000）
    if(PrintJobState::instance().snapshot().sent_packets <= sent_at_layer_ready_) {
        return false;
    }
    // 进入换层等待状态，等待当前层运动包全部下发并 MCU 缓存排空
    waiting_layer_.store(true);
    layer_drain_pending_.store(true);
    layer_drain_since_ = std::chrono::steady_clock::now();
    serial_evt_condition_ok_.store(false);
    serial_evt_print_ready_.store(false);
    serial_handshake_phase_.store(SERIAL_HS_NONE);
    return true;
#else
    (void)mark;
    return false;
#endif
}

void McuLink::link_try_layer_serial_dispatch()
{
#if !GLAVO_ENABLE_LAYER_CHANGE
    return;
#endif
    if(!waiting_layer_.load() || !layer_drain_pending_.load()) {
        return; // 不处于换层排空等待状态
    }
    if(serial_fd_ < 0) {
        // 无串口时直接跳过换层等待
        waiting_layer_.store(false);
        layer_drain_pending_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
        return;
    }
    // 本层须已下发过扫描指令，才允许发单层扫描完成（协议：扫描 → 0x05）
    const int layer_packets =
        PrintJobState::instance().snapshot().sent_packets - sent_at_layer_ready_;
    if(layer_packets <= 0) {
        return;
    }
    // 必须等 MCU 缓存完全排空，确保当前层最后一包已被执行
    if(!mcu_rates_drained(layer_packets, layer_drain_since_)) {
        return;
    }
    if(!mcu_rates_seen_nonempty_.load()) {
        /* 兜底放行：本层太薄，MCU 在两帧心跳之间就跑空了，缓存非空从未被观测到 */
        UPC_LOG_MCU_LINK(Warn,
            "layer drain fallback: rates stayed 0 for the whole layer "
            "(layer=%u um packets=%d)",
            static_cast<unsigned>(active_layer_height_um_), layer_packets);
    }

#if GLAVO_ENABLE_LAYER_SNAP
    PrintVideoNotify::instance().on_layer_snap(active_layer_height_um_);
    current_layer_snapped_ = true;
#endif

    // 发送 layer_done 帧，通知 MCU 当前层已结束
    send_serial_layer_done(active_layer_height_um_);
    layer_drain_pending_.store(false);
    // 转入等待 PRINT_READY 状态
    serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
    serial_evt_print_ready_.store(false);

    /* 换层铺粉期间不下发运动包，link_send_one_motion_batch 不会触发进度回调。
     * 纯层进度以 Snap 计数为分子，这里补推一次，台阶才会在本层结束时立刻出现，
     * 而不是等下一层开始发包；同时刷新载荷里的环境字段。 */
    if(progress_cb_) {
        progress_cb_();
    }
}

/* ---------- 运动包批量下发 ---------- */

int McuLink::link_send_one_motion_batch()
{
    if(user_paused_.load() || o2_hold_.load()) {
        return 0; // 用户暂停或 SOC 氧过高：不下发 TCP 运动包
    }
    if(tcp_stage_count() == 0) {
        return 0; // 暂存队列为空
    }

    parser::rb_packet_t staged[kMaxMotionBatch];
    int n = 0;
    enum class BatchKind : uint8_t {
        None = 0,
        Motion,
        Power,
    };
    BatchKind batch_kind = BatchKind::None;

    // 从 tcp_stage_ 头部连续取包：LayerMark 留给外层；未知类型丢弃。
    // 规则：
    // 1) 普通运动包：最多取 current_tcp_batch_size()；
    // 2) 功率包(0x02)：连续功率包合并为同一批（方案 B）。
    const int batch_limit = current_tcp_batch_size();
    while(n < kMaxMotionBatch) {
        parser::rb_pkt_type_t ptype;
        if(!tcp_stage_peek_type(&ptype)) {
            break; // 队列为空
        }
        if(ptype == parser::RB_PKT_LAYER_MARK) {
            break; // 遇到换层标记，留待外层处理
        }
        if(ptype != parser::RB_PKT_MOTION) {
            // 遇到未知类型包，丢弃并终止本轮批量
            parser::rb_packet_t discard;
            tcp_stage_pop_front(&discard);
            break;
        }

        bool front_is_power = false;
        {
            std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
            if(tcp_stage_.empty()) {
                break;
            }
            front_is_power = (tcp_stage_.front().data[0] == 0x02u);
        }
        if(batch_kind == BatchKind::None) {
            batch_kind = front_is_power ? BatchKind::Power : BatchKind::Motion;
        }
        if((batch_kind == BatchKind::Power && !front_is_power) ||
           (batch_kind == BatchKind::Motion && front_is_power)) {
            break; // 分批边界：功率包与普通运动包不混发
        }
        if(batch_kind == BatchKind::Motion && n >= batch_limit) {
            break;
        }

        parser::rb_packet_t rp;
        if(!tcp_stage_pop_front(&rp)) {
            break;
        }
        staged[n++] = rp;
    }

    if(n <= 0) {
        return 0;
    }

    auto requeue_staged = [&]() {
        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        for(int i = n - 1; i >= 0; i--) {
            tcp_stage_.push_front(staged[i]);
        }
    };
    auto terminate_on_fail = [&](const char* reason) {
        UPC_LOG_MCU_LINK(Error, "%s", reason);
        if(!terminate_pending_.load()) {
            PrintJobState::instance().set_job_error(true);
            request_terminate();
        }
    };
    auto send_staged_as_motion_frame = [&]() -> int {
        uint8_t payload[kMaxMotionBatch * 11];
        for(int i = 0; i < n; i++) {
            std::memcpy(&payload[i * 11], staged[i].data, 11);
        }
        uint8_t frame[3 + kMaxMotionBatch * 11];
        const int flen = build_motion_frame(payload, n, frame, sizeof(frame));
        if(flen <= 0) {
            return -3;
        }
        return tcp_send_raw(frame, static_cast<size_t>(flen));
    };
    auto log_power_packets_if_changed = [&]() {
        if(!laser_power_log_enabled()) {
            return;
        }
        for(int i = 0; i < n; i++) {
            if(staged[i].data[0] != 0x02u) {
                continue;
            }
            const uint16_t raw = static_cast<uint16_t>(
                static_cast<uint16_t>(staged[i].data[9]) |
                (static_cast<uint16_t>(staged[i].data[10]) << 8u));
            if(laser_power_log_valid_ && last_laser_power_raw_ == raw) {
                continue;
            }
            UPC_LOG_MCU_LINK_TO(LaserPower, Info, "laser_power_set raw=%u",
                             static_cast<unsigned>(raw));
            last_laser_power_raw_ = raw;
            laser_power_log_valid_ = true;
        }
    };

    if(batch_kind == BatchKind::Power) {
        // 功率变更窗口：
        // 临时暂停(0x0A pause=1) -> 连续功率包(0x02) -> 等 10ms -> 条件恢复(0x0A pause=0)
        if(terminate_pending_.load()) {
            return 0;
        }
        if(!send_tcp_emergency_ctrl(false, true)) {
            terminate_on_fail("power batch: send pause ctrl failed");
            return 0;
        }
        if(terminate_pending_.load()) {
            return 0;
        }
        const int send_rc = send_staged_as_motion_frame();
        if(send_rc != 0) {
            terminate_on_fail("power batch: send power frame failed");
            return 0;
        }
        log_power_packets_if_changed();

        constexpr int kPowerResumeDelayMs = 10;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kPowerResumeDelayMs);
        while(std::chrono::steady_clock::now() < deadline) {
            if(terminate_pending_.load()) {
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(terminate_pending_.load()) {
            return 0;
        }
        if(!user_paused_.load()) {
            if(!send_tcp_emergency_ctrl(false, false)) {
                terminate_on_fail("power batch: send resume ctrl failed");
                return 0;
            }
        } else {
            UPC_LOG_MCU_LINK(Info,
                             "power batch: keep paused after delay (user paused=1)");
        }
    } else {
        const int send_rc = send_staged_as_motion_frame();
        if(send_rc != 0) {
            /* -1：未发出任何字节，可整批回退重试；
             * -2：半帧已出 / hangup，禁止重发，依赖 terminate 清队列重同步。 */
            if(send_rc == -1 && tcp_fd_ >= 0) {
                requeue_staged();
            } else {
                UPC_LOG_MCU_LINK(Error,
                    "motion batch drop (no rebatch) n=%d rc=%d connected=%d",
                    n, send_rc, tcp_fd_ >= 0 ? 1 : 0);
                if(!terminate_pending_.load()) {
                    PrintJobState::instance().set_job_error(true);
                    request_terminate();
                }
            }
            return 0;
        }
    }

    // 更新已发送包计数
    const int sent = PrintJobState::instance().snapshot().sent_packets + n;
    PrintJobState::instance().set_sent_packets(sent);

    // 触发进度回调
    if(progress_cb_) {
        progress_cb_();
    }
    return n;
}

/* ---------- 链路主逻辑：运动队列排空与完成检测 ---------- */

void McuLink::link_drain_motion_queue()
{
    if(!is_sending_.load()) {
        return; // 未开始发送
    }

    // 1. 从 RingBuffer 预取包到 tcp_stage_
    refill_tcp_stage_from_ring();
    // 2. 本层指令已发完且 MCU 缓存排空时，尝试下发单层扫描完成（0x05）
    link_try_layer_serial_dispatch();

    // 3. 换层等待 / 首层握手等待：仅暂停 TCP 运动下发，不阻塞 0x05 排空检测
    if(waiting_layer_.load()) {
        return;
    }

    // 4. 背压检查：若 MCU 指令缓存占用率超过阈值，暂停运动包下发
    if(motion_stream_backpressured()) {
        return;
    }

    // 5. 处理换层标记：若头部是 LayerMark，进入换层流程
    if(link_handle_layer_mark()) {
        link_try_layer_serial_dispatch();
        return;
    }

    // 6. 若暂存队列为空，本次无可下发内容
    if(tcp_stage_count() == 0) {
        return;
    }

    // 7. 批量发送运动包
    link_send_one_motion_batch();
}

void McuLink::link_try_finish_sending()
{
    if(completing_cleanup_.load()) {
        return;
    }
    if(user_paused_.load() || o2_hold_.load()) {
        return; // 用户暂停或 SOC 氧暂停：MCU 可能仍有缓存，恢复后再判完成
    }
    if(!is_sending_.load()) {
        return; // 未发送中
    }

    const bool waiting_layer = waiting_layer_.load();
    if(waiting_layer && layer_drain_pending_.load()) {
        return; // 仍在等待发送 layer_done，不能判自然完成
    }
    if(tcp_stage_count() > 0) {
        return; // 暂存队列仍有数据待下发
    }
    if(rb_ && !rb_->empty()) {
        return; // RingBuffer 仍有数据
    }
    parser::IParserWorker* parser = active_parser();
    if(!parser) {
        return;
    }
    if(parser->is_parsing()) {
        return; // 解析器仍在解析，可能还有新数据产生
    }
    if(!parser->is_file_read_exhausted()) {
        return; // 文件尚未读完
    }

    /* 尾层兜底：文件已耗尽且无待发数据时，若仅剩“等待下一次 PRINT_READY”，
     * 说明没有后续层可下发，允许从 waiting_layer 退出来执行自然完成。 */
    if(waiting_layer) {
        UPC_LOG_MCU_LINK(Info,
            "tail layer wait drained: finalize without next PRINT_READY");
        waiting_layer_.store(false);
        layer_drain_pending_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
        serial_evt_condition_ok_.store(false);
        serial_evt_print_ready_.store(false);
    }

    const auto now = std::chrono::steady_clock::now();

    // 所有运动包已下发，但需等 MCU 缓存排空才算真正完成
    if(!mcu_rates_drained(PrintJobState::instance().snapshot().sent_packets,
                          natural_complete_since_)) {
        if(!natural_complete_pending_.load()) {
            natural_complete_since_ = now;
        }
        natural_complete_pending_.store(true);
        poll_mcu_drain_heartbeat();

        if(MCU_DRAIN_TIMEOUT_MS > 0 &&
           natural_complete_since_.time_since_epoch().count() != 0 &&
           now - natural_complete_since_ >=
               std::chrono::milliseconds(MCU_DRAIN_TIMEOUT_MS)) {
            const auto snap = PrintJobState::instance().snapshot();
            UPC_LOG_MCU_LINK_TO(TcpHeartbeat, Warn,
                "finish wait: drain still pending after %d ms cmd_rate=%u data_rate=%u "
                "(keep waiting; UI exits only when rates==0)",
                MCU_DRAIN_TIMEOUT_MS,
                static_cast<unsigned>(snap.cmd_cache_rate),
                static_cast<unsigned>(snap.data_cache_rate));
            /* 避免日志刷屏：将计时原点前移一个超时窗口 */
            natural_complete_since_ = now;
        }
        return;
    }

    // 真正自然完成：执行清理并上报事件
    natural_complete_pending_.store(false);
    natural_complete_since_ = {};
    is_sending_.store(false);
    run_natural_complete_cleanup();
}

/* ---------- 链路线程休眠策略 ---------- */

int McuLink::link_thread_sleep_ms() const
{
    if(ota_session_.is_active() || serial_ota_session_.is_active()) {
        return current_tcp_send_interval_ms();
    }
    if(terminate_pending_.load()) {
        return 5; // 终止请求需快速响应
    }
    if(is_sending_.load() && !waiting_layer_.load()) {
        return current_tcp_send_interval_ms(); // 活跃下发期间按固定间隔轮询
    }
    if(waiting_layer_.load() || natural_complete_pending_.load()) {
        return 10; // 换层等待或完成等待，稍快轮询
    }
    if(tcp_fd_ >= 0) {
        return current_tcp_heartbeat_interval_ms(); // 空闲但有连接，按心跳间隔休眠
    }
    return 50; // 无连接时的默认轮询间隔
}

/* ---------- OTA（测试） ---------- */

void McuLink::set_ota_status_callback(McuOtaStatusCb cb)
{
    ota_session_.set_status_callback(std::move(cb));
}

void McuLink::set_serial_ota_status_callback(McuOtaStatusCb cb)
{
    serial_ota_session_.set_status_callback(std::move(cb));
}

bool McuLink::start_mcu_ota(const char* image_dir)
{
    if(!is_tcp_connected()) {
        UPC_LOG_MCU_OTA(Error, "OTA start: TCP not connected");
        return false;
    }
    if(is_sending_.load()) {
        UPC_LOG_MCU_OTA(Warn, "OTA start: print active");
        return false;
    }
    if(serial_ota_session_.is_active()) {
        UPC_LOG_MCU_OTA(Warn, "OTA start: serial ota active");
        return false;
    }
    return ota_session_.start(image_dir);
}

bool McuLink::start_serial_mcu_ota(const char* image_dir)
{
    if(serial_fd_ < 0) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA start: serial not connected");
        return false;
    }
    if(is_sending_.load()) {
        UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA start: print active");
        return false;
    }
    if(ota_session_.is_active()) {
        UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA start: glavo ota active");
        return false;
    }
    if(!serial_ota_session_.start(image_dir)) {
        return false;
    }
    /* MCU 升级后会复位，下一份真实版本上报再发一次上电 0x12。 */
    serial_laser_off_applied_.store(false);
    serial_laser_off_pending_.store(false);
    reassert_red_after_laser_off_.store(false);
    {
        std::lock_guard<std::mutex> lock(serial_tx_mtx_);
        serial_tx_queue_.clear();
    }
    serial_clear_reliable();
    return true;
}

void McuLink::cancel_mcu_ota()
{
    ota_session_.cancel();
}

void McuLink::cancel_serial_mcu_ota()
{
    serial_ota_session_.cancel();
}

bool McuLink::ota_active() const
{
    return ota_session_.is_active();
}

bool McuLink::serial_ota_active() const
{
    return serial_ota_session_.is_active();
}

void McuLink::on_tcp_status_response(const tcp_response_t& resp)
{
    last_tcp_hb_rsp_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count());
    ota_session_.on_status_response(resp);
#if GLAVO_OTA_POST_RECONNECT_ENABLE
    if(ota_session_.take_post_success_reconnect()) {
        ota_post_reconnect_.store(true);
        UPC_LOG_MCU_OTA(Info, "OTA success: schedule TCP post-reconnect");
    }
#endif
    if(!ota_session_.is_active()) {
        const auto prev = PrintJobState::instance().snapshot();
        if(resp.cmd_rate > static_cast<uint16_t>(MCU_DRAIN_MAX_RATE) ||
           resp.data_rate > static_cast<uint16_t>(MCU_DRAIN_MAX_RATE)) {
            mcu_rates_seen_nonempty_.store(true);
        }
        PrintJobState::instance().set_mcu_rates(resp.cmd_rate, resp.data_rate);
        if(tcp_hb_rsp_log_enabled() && print_task_flow_active_.load()) {
            UPC_LOG_MCU_LINK_TO(TcpHeartbeat, Debug,
                "tcp hb rsp cmd_rate=%u data_rate=%u event=0x%02X ota=%u",
                static_cast<unsigned>(resp.cmd_rate),
                static_cast<unsigned>(resp.data_rate),
                static_cast<unsigned>(resp.event_flag),
                static_cast<unsigned>(resp.ota_progress));
        }

        const bool rates_changed =
            prev.cmd_cache_rate != resp.cmd_rate ||
            prev.data_cache_rate != resp.data_rate;
        if(natural_complete_pending_.load() && progress_cb_ && rates_changed) {
            progress_cb_();
        }
    }
}

void McuLink::on_tcp_init_report(bool has_version, uint32_t fw_version_le,
                                 uint8_t firmware_id, uint8_t ota_state,
                                 uint8_t selfcheck_boot_count)
{
    const bool use_small_mem =
        has_version && (fw_version_le >= GLAVO_SMALL_MEM_FW_VER_MIN);
    const int desired_profile = use_small_mem ? 1 : 0;
    const bool current_small = tcp_small_mem_profile_.load();
    const int current_profile = current_small ? 1 : 0;

    UPC_LOG_MCU_LINK(Info,
        "tcp init report: has_ver=%d fw=0x%08X fw_id=%u ota=0x%02X selfchk=%u target=%s",
        has_version ? 1 : 0,
        static_cast<unsigned>(fw_version_le),
        static_cast<unsigned>(firmware_id),
        static_cast<unsigned>(ota_state),
        static_cast<unsigned>(selfcheck_boot_count),
        use_small_mem ? "small-mem" : "legacy");

    /* About 页 Galvo Version：缓存并主动推 UI（旧 Length=0x01 包无版本则不覆盖已有缓存） */
    if(has_version) {
        tcp_fw_version_le_.store(fw_version_le);
        tcp_firmware_id_.store(firmware_id);
        tcp_fw_valid_.store(true);
        publish_galvo_version_ntf();
    }

    if(desired_profile == current_profile) {
        return;
    }

    const bool busy =
        is_sending_.load() || start_pending_.load() || waiting_layer_.load() ||
        natural_complete_pending_.load() || terminate_pending_.load() ||
        completing_cleanup_.load();
    if(busy) {
        tcp_pending_profile_.store(desired_profile);
        UPC_LOG_MCU_LINK(Info,
            "tcp profile defer: busy=1 current=%s pending=%s fw=0x%08X",
            current_small ? "small-mem" : "legacy",
            use_small_mem ? "small-mem" : "legacy",
            static_cast<unsigned>(fw_version_le));
        return;
    }

    apply_tcp_profile(use_small_mem, "tcp_init", fw_version_le);
}

void McuLink::publish_galvo_version_ntf()
{
    middleware::GalvoVersionPayload p{};
    p.valid         = tcp_fw_valid_.load() ? 1 : 0;
    p.firmware_id   = tcp_firmware_id_.load();
    p.fw_version_le = tcp_fw_version_le_.load();

    auto send_to = [&](middleware::ModuleId target) {
        middleware::Message ntf(middleware::MsgType::GLAVO_Galvo_Version_Ntf,
            middleware::ModuleId::APP_GLAVO, target);
        ntf.priority = middleware::MsgPriority::High;
        ntf.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        if(!middleware::MsgPort::instance().publish_async(ntf)) {
            UPC_LOG_MCU_LINK(Warn, "GLAVO_Galvo_Version_Ntf publish_async failed target=%u",
                static_cast<unsigned>(target));
        }
    };
    send_to(middleware::ModuleId::UI);
    send_to(middleware::ModuleId::APP_MQTT);
}

void McuLink::publish_simulated_versions_if_unknown()
{
    /* 仅在未知时补值：真实上报到达后会覆盖，不影响真实链路。 */
    if(!mcu_fw_valid_.load()) {
        middleware::McuVersionPayload p{};
        p.valid = 1;
        p.major = 1;
        p.minor = 0;
        p.patch = 0;
        p.laser_type = 0;
        p.z_motor_type = 0;
        {
            std::lock_guard<std::mutex> lock(mcu_fw_mtx_);
            mcu_fw_cache_ = p;
        }
        mcu_fw_valid_.store(true);
        publish_mcu_version_ntf();
        UPC_LOG_MCU_LINK(Info, "env_sim version fallback: MCU=1.0.0.0.0");
    }

    if(!tcp_fw_valid_.load()) {
        constexpr uint32_t kSimGalvoFwLe = 0x00030004u; /* -> 3.0.4 */
        tcp_fw_version_le_.store(kSimGalvoFwLe);
        tcp_firmware_id_.store(1u);
        tcp_fw_valid_.store(true);
        publish_galvo_version_ntf();
        UPC_LOG_MCU_LINK(Info, "env_sim version fallback: Galvo=0x%08X",
                         static_cast<unsigned>(kSimGalvoFwLe));
    }
}

int McuLink::current_tcp_batch_size() const noexcept
{
    int v = static_cast<int>(tcp_batch_size_.load());
    if(v < 1) {
        v = 1;
    }
    if(v > kMaxMotionBatch) {
        v = kMaxMotionBatch;
    }
    return v;
}

int McuLink::current_tcp_send_interval_ms() const noexcept
{
    int v = static_cast<int>(tcp_send_interval_ms_.load());
    if(v < 1) {
        v = 1;
    }
    return v;
}

int McuLink::current_tcp_heartbeat_interval_ms() const noexcept
{
    int v = static_cast<int>(tcp_heartbeat_interval_ms_.load());
    if(v < 1) {
        v = 1;
    }
    return v;
}

uint32_t McuLink::current_cmd_backpressure_pct() const noexcept
{
    uint32_t v = static_cast<uint32_t>(tcp_cmd_backpressure_pct_.load());
    if(v > 100u) {
        v = 100u;
    }
    return v;
}

void McuLink::try_apply_pending_tcp_profile()
{
    const int pending = tcp_pending_profile_.load();
    if(pending < 0) {
        return;
    }
    const bool busy =
        is_sending_.load() || start_pending_.load() || waiting_layer_.load() ||
        natural_complete_pending_.load() || terminate_pending_.load() ||
        completing_cleanup_.load();
    if(busy) {
        return;
    }

    tcp_pending_profile_.store(-1);
    apply_tcp_profile(pending == 1, "deferred", 0u);
}

void McuLink::apply_tcp_profile(bool small_mem, const char* reason,
                                uint32_t fw_version_le)
{
    const uint16_t batch_cfg = static_cast<uint16_t>(small_mem
        ? GLAVO_SMALL_MEM_TCP_BATCH_SIZE
        : TCP_BATCH_SIZE);
    const uint16_t send_cfg = static_cast<uint16_t>(small_mem
        ? GLAVO_SMALL_MEM_TCP_SEND_INTERVAL_MS
        : TCP_SEND_INTERVAL_MS);
    const uint16_t hb_cfg = static_cast<uint16_t>(small_mem
        ? GLAVO_SMALL_MEM_TCP_HEARTBEAT_INTERVAL_MS
        : TCP_HEARTBEAT_INTERVAL_MS);
    const uint8_t bp_cfg = static_cast<uint8_t>(small_mem
        ? GLAVO_SMALL_MEM_MCU_CMD_BACKPRESSURE_PCT
        : MCU_CMD_BACKPRESSURE_PCT);

    uint16_t batch = batch_cfg < 1 ? 1 : batch_cfg;
    if(batch > static_cast<uint16_t>(kMaxMotionBatch)) {
        batch = static_cast<uint16_t>(kMaxMotionBatch);
    }
    const uint16_t send_ms = send_cfg < 1 ? 1 : send_cfg;
    const uint16_t hb_ms = hb_cfg < 1 ? 1 : hb_cfg;
    uint8_t bp_pct = bp_cfg > 100 ? 100 : bp_cfg;

    tcp_batch_size_.store(batch);
    tcp_send_interval_ms_.store(send_ms);
    tcp_heartbeat_interval_ms_.store(hb_ms);
    tcp_cmd_backpressure_pct_.store(bp_pct);
    tcp_small_mem_profile_.store(small_mem);

    UPC_LOG_MCU_LINK(Info,
        "tcp profile apply(%s): mode=%s fw=0x%08X batch=%u send_ms=%u hb_ms=%u bp=%u%%",
        reason ? reason : "n/a",
        small_mem ? "small-mem" : "legacy",
        static_cast<unsigned>(fw_version_le),
        static_cast<unsigned>(batch),
        static_cast<unsigned>(send_ms),
        static_cast<unsigned>(hb_ms),
        static_cast<unsigned>(bp_pct));
}

void McuLink::ota_tick()
{
    ota_session_.tick(
        +[](const uint8_t* data, size_t len) -> int {
            return McuLink::instance().tcp_send_raw(data, len);
        },
        +[]() { McuLink::instance().tcp_drain_rx(); },
        tcp_fd_ >= 0);
}

void McuLink::serial_ota_tick()
{
    serial_ota_session_.tick(
        +[](const uint8_t* data, size_t len) -> int {
            return McuLink::instance().serial_send_raw(data, len);
        },
        serial_fd_ >= 0);
}

void McuLink::link_try_ota_post_reconnect()
{
    const bool ota_pending = ota_post_reconnect_.load();
    if(ota_pending && ota_session_.is_active()) {
        return;
    }

    const int rc = connect_tcp();
    if(rc == 0) {
        if(ota_pending) {
            ota_post_reconnect_.store(false);
            UPC_LOG_MCU_OTA(Info, "OTA post-reconnect TCP OK %s:%d",
                            GLAVO_TCP_DEFAULT_IP, GLAVO_TCP_DEFAULT_PORT);
        }
        return;
    }

    if(ota_pending) {
        UPC_LOG_MCU_OTA(Warn, "OTA post-reconnect TCP fail %s:%d rc=%d",
                        GLAVO_TCP_DEFAULT_IP, GLAVO_TCP_DEFAULT_PORT, rc);
    }
}

/* ---------- 链路线程主循环 ---------- */

void McuLink::link_thread_fn()
{
    using clock = std::chrono::steady_clock;
    auto next_hb = clock::now(); // 下次心跳发送时间点
    auto next_tcp_retry = clock::now(); // 下次 TCP 重连尝试时间点
    bool tcp_state_published = false;
    bool last_tcp_connected = false;
    constexpr int64_t kTcpLivenessTimeoutFloorMs = 1500;

    while(link_run_.load()) {
        const auto now = clock::now();
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now.time_since_epoch()).count();
        const bool tcp_connected_now = (tcp_fd_ >= 0);

        /* UI 顶栏 Ready/Idle 需要直接反映 TCP 链路状态，不依赖传感器上报频率。
         * 线程启动先推一次，之后仅在连接态变化时推送。 */
        if(!tcp_state_published || tcp_connected_now != last_tcp_connected) {
            publish_env_status_ntf_ui_only();
            const bool tcp_just_connected =
                tcp_connected_now && (!tcp_state_published || !last_tcp_connected);
            if(tcp_just_connected) {
                force_switch_to_red_mode_on_tcp_connect();
            }
            tcp_state_published = true;
            last_tcp_connected = tcp_connected_now;
        }

        // ---------- 串口 TX/RX/可靠重传：独立于 TCP，始终处理 ----------
        // 串口（MCU 调试链路）与 TCP（振镜）相互独立；即使振镜未连接，
        // 调试命令（UI_Debug_*）与传感器上报也必须照常收发。
        drain_serial_tx_queue();
        maybe_reassert_red_after_serial_laser_off();
        if(serial_laser_off_pending_.load()) {
            apply_serial_laser_off_after_mcu_ready();
        }
        serial_drain_rx();
        link_process_serial_reliable_tx();
        poll_env_sim_tick();
        poll_soc_env_tick();

        // ---------- TCP RX 排空与断线检测/重连（不阻塞串口） ----------
        if(tcp_fd_ >= 0) {
            tcp_drain_rx();
            if(!ota_session_.is_active() && !serial_ota_session_.is_active()) {
                const int64_t hb_timeout_ms = std::max<int64_t>(
                    kTcpLivenessTimeoutFloorMs,
                    static_cast<int64_t>(current_tcp_heartbeat_interval_ms()) * 8);
                const int64_t last_hb_rsp_ms = last_tcp_hb_rsp_ms_.load();
                if(last_hb_rsp_ms > 0 &&
                   now_ms - last_hb_rsp_ms > hb_timeout_ms) {
                    UPC_LOG_MCU_LINK(Warn,
                        "TCP liveness timeout: no heartbeat rsp for %lld ms (timeout=%lld), force reconnect",
                        static_cast<long long>(now_ms - last_hb_rsp_ms),
                        static_cast<long long>(hb_timeout_ms));
                    close(tcp_fd_);
                    tcp_fd_ = -1;
                    tcp_rx_accum_len_ = 0;
                }
            }
        } else if(is_sending_.load() && !ota_session_.is_active() &&
                   !serial_ota_session_.is_active() &&
                   !completing_cleanup_.load()) {
            // TCP 在发送过程中断开：标记错误并触发终止流程
            // （自然完成 cleanup 中的 Reset 断线忽略）
            PrintJobState::instance().set_job_error(true);
            is_sending_.store(false);
            terminate_pending_.store(true);
        } else if(!terminate_pending_.load() && !ota_session_.is_active() &&
                  !serial_ota_session_.is_active()) {
            // 空闲断线或 OTA 成功后：节流式 connect_tcp（固定 IP/PORT）
            if(tcp_fd_ < 0 || ota_post_reconnect_.load()) {
                if(ota_post_reconnect_.load() || now >= next_tcp_retry) {
                    link_try_ota_post_reconnect();
                    next_tcp_retry =
                        now + std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS);
                }
            }
        }

        if(ota_session_.is_active()) {
            ota_tick();
        } else if(serial_ota_session_.is_active()) {
            serial_ota_tick();
        } else if(terminate_pending_.load()) {
            /* 只有真正跑完清理才清挂起位：CAS 失败时保留，下一轮继续重试，
             * 否则终止请求会被吃掉（作业不停、UI 等不到 TerminateDone）。 */
            if(run_manual_terminate_cleanup()) {
                terminate_pending_.store(false);
            }
        } else {
            try_apply_pending_tcp_profile();
            // ---------- 心跳（空闲/用户暂停/背压时都要刷 MCU rate） ----------
            const bool stream = motion_stream_active();
            const bool backpressured = motion_stream_backpressured();
            const bool need_hb =
                (!stream || (is_sending_.load() &&
                             (user_paused_.load() || o2_hold_.load())) ||
                 backpressured) &&
                !ota_session_.suppress_idle_heartbeat() &&
                tcp_fd_ >= 0 && now >= next_hb;
            if(need_hb) {
                uint8_t hb[64];
                const int hblen = build_heartbeat_frame(hb, sizeof(hb));
                if(hblen > 0) {
                    tcp_send_raw(hb, (size_t)hblen);
                }
                next_hb = now + std::chrono::milliseconds(
                    current_tcp_heartbeat_interval_ms());
                tcp_drain_rx(); // 心跳发送后再排空一次 RX
            }

            // ---------- 串口换层握手（打印流程相关） ----------
            link_process_serial_handshake();
            link_process_o2_print_gate();

            // ---------- 开始发送（处理挂起的 begin_sending） ----------
            try_complete_pending_start();

            // ---------- 运动包下发 ----------
            link_drain_motion_queue();

            // ---------- 完成检测 ----------
            link_try_finish_sending();
        }

        // ---------- 休眠等待下一轮 ----------
        std::this_thread::sleep_for(
            std::chrono::milliseconds(link_thread_sleep_ms()));
    }
}

} // namespace upc::glavo

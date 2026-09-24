#ifndef SPI_STATUS_LED_HPP
#define SPI_STATUS_LED_HPP

/**
 * @file spi_status_led.hpp
 * @brief SPI 灯效板（/dev/spidev1.0，40 颗 WS）按设备状态刷单色。
 *
 * 优先级：MCU CONDITION+ACTIVE 红 > print_running_ 橙 > 空闲蓝。
 * 进程启动即打开；节点缺失只 Warn 一次。
 *
 * 线程：业务侧（含 McuLink 串口线程）只改状态并排队；专用 led 线程做
 * open/ioctl，避免持锁 SPI 拖慢心跳/报警路径。
 */

#include "serial_alarm.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace upc {
namespace glavo {

class SpiStatusLed {
public:
    static SpiStatusLed& instance();

    /** 打开灯板；false 关 fd，不再发色。 */
    void set_enabled(bool on);

    /** Confirm 打印会话（含测高/光路/暂停）。 */
    void set_printing(bool on);

    /**
     * @brief 调试页强制单色；force=false 释放接管回自动推导。
     *
     * 不看 enabled_（绕开 MCU major≥4 版本门），且每次调用必发一帧，便于台架验色。
     * color 取 DebugStatusLedColor；仅 Raw 用 r/g/b，其余三色用本文件的常量。
     */
    void set_debug_force(bool force, uint8_t color,
                         uint8_t r = 0, uint8_t g = 0, uint8_t b = 0);

    /** 调试态回读：force=false 时 color/rgb 无意义。 */
    void debug_force_state(bool& force, uint8_t& color,
                           uint8_t& r, uint8_t& g, uint8_t& b);

    void on_alarm_report(const serial_alarm_report_t& report);
    void on_alarm_snapshot(const serial_alarm_snapshot_t& snap);

private:
    SpiStatusLed() = default;
    ~SpiStatusLed();
    SpiStatusLed(const SpiStatusLed&) = delete;
    SpiStatusLed& operator=(const SpiStatusLed&) = delete;

    /** 与 middleware::DebugStatusLedColor 同序；Raw 仅调试接管可达。 */
    enum class Color : uint8_t { Blue = 0, Orange = 1, Red = 2, Raw = 3 };

    void ensure_worker_locked();
    void worker_loop();
    void stop_worker();
    /** 推导目标色并排队；force_send=true 跳过同色去重。调用方须已持 mtx_。 */
    void queue_apply_locked(bool force_send = false);
    /** 排队刷空闲蓝后关 fd（释放调试接管且未 enabled 时）。调用方须已持 mtx_。 */
    void queue_idle_blue_and_close_locked();
    void commit_snapshot_round_locked();
    bool ensure_open_io();
    void close_io();
    bool send_rgb_io(uint8_t r, uint8_t g, uint8_t b);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool worker_started_ = false;
    bool worker_stop_ = false;

    bool enabled_ = false;
    bool printing_ = false;
    bool open_warn_done_ = false;
    Color last_color_ = Color::Blue;
    bool last_sent_ = false;

    /* 调试页验色接管：优先于 alarm/printing 推导，且不受 enabled_ 约束 */
    bool dbg_force_ = false;
    Color dbg_color_ = Color::Blue;
    uint8_t dbg_r_ = 0, dbg_g_ = 0, dbg_b_ = 0;

    std::unordered_set<uint16_t> condition_active_;

    uint8_t snap_expect_ = 0;
    uint16_t snap_seen_ = 0; /**< bit i = slot i 已收到 */
    uint32_t snap_active_[SERIAL_ALARM_SNAPSHOT_SLOTS]{};

    /* led 线程消费的 pending（mtx_ 保护） */
    bool dirty_ = false;
    bool close_after_send_ = false;
    uint8_t pending_r_ = 0, pending_g_ = 0, pending_b_ = 0;
    Color pending_color_ = Color::Blue;

    /* fd 仅 led 线程触碰 */
    int fd_ = -1;
};

} // namespace glavo
} // namespace upc

#endif

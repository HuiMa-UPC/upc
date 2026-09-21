#ifndef SPI_STATUS_LED_HPP
#define SPI_STATUS_LED_HPP

/**
 * @file spi_status_led.hpp
 * @brief SPI 灯效板（/dev/spidev1.0，40 颗 WS）按设备状态刷单色。
 *
 * 优先级：MCU CONDITION+ACTIVE 红 > print_running_ 橙 > 空闲蓝。
 * 进程启动即打开；节点缺失只 Warn 一次。
 */

#include "serial_alarm.hpp"

#include <cstdint>
#include <mutex>
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

    void on_alarm_report(const serial_alarm_report_t& report);
    void on_alarm_snapshot(const serial_alarm_snapshot_t& snap);

private:
    SpiStatusLed() = default;
    ~SpiStatusLed();
    SpiStatusLed(const SpiStatusLed&) = delete;
    SpiStatusLed& operator=(const SpiStatusLed&) = delete;

    enum class Color : uint8_t { Blue = 0, Orange = 1, Red = 2 };

    bool ensure_open_locked();
    void close_locked();
    void apply_locked();
    bool send_rgb_locked(uint8_t r, uint8_t g, uint8_t b);
    void commit_snapshot_round_locked();

    std::mutex mtx_;
    int fd_ = -1;
    bool enabled_ = false;
    bool printing_ = false;
    bool open_warn_done_ = false;
    Color last_color_ = Color::Blue;
    bool last_sent_ = false;

    std::unordered_set<uint16_t> condition_active_;

    uint8_t snap_expect_ = 0;
    uint16_t snap_seen_ = 0; /**< bit i = slot i 已收到 */
    uint32_t snap_active_[SERIAL_ALARM_SNAPSHOT_SLOTS]{};
};

} // namespace glavo
} // namespace upc

#endif

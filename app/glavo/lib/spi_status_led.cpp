#include "spi_status_led.hpp"
#include "device_config.hpp"

#include "debug/rs_log.hpp"

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#endif

#include <cerrno>
#include <cstring>

namespace upc {
namespace glavo {
namespace {

constexpr uint8_t kLedNum = 40;
constexpr uint8_t kBytesPerLed = 12;
constexpr uint8_t kResetBytes = 100;
constexpr size_t kBufSize = static_cast<size_t>(kLedNum) * kBytesPerLed + kResetBytes;
constexpr uint32_t kWsHz = 3200000;
constexpr uint8_t kNib[2] = {0x8, 0xE};

// constexpr uint8_t kBlueR = 167, kBlueG = 194, kBlueB = 227;
constexpr uint8_t kBlueR = 30, kBlueG = 30, kBlueB = 30;
constexpr uint8_t kOrangeR = 50, kOrangeG = 255, kOrangeB = 0;
constexpr uint8_t kRedR = 255, kRedG = 0, kRedB = 0;

void enc_byte(uint8_t* buf, uint32_t* p, uint8_t v)
{
    for(int i = 0; i < 8; i += 2) {
        const uint8_t hi = kNib[(v >> (7 - i)) & 1];
        const uint8_t lo = kNib[(v >> (6 - i)) & 1];
        buf[(*p)++] = static_cast<uint8_t>((hi << 4) | lo);
    }
}

/** 与 middleware::alarm_snapshot_code_of 同式，避免本模块依赖契约头。 */
uint16_t snapshot_code_of(uint8_t slot_id, uint8_t bit)
{
    return static_cast<uint16_t>(1000 + (slot_id / 3) * 100 + (slot_id % 3) * 32 + bit);
}

} // namespace

SpiStatusLed& SpiStatusLed::instance()
{
    static SpiStatusLed inst;
    return inst;
}

SpiStatusLed::~SpiStatusLed()
{
    std::lock_guard<std::mutex> lock(mtx_);
    close_locked();
}

void SpiStatusLed::set_enabled(bool on)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(enabled_ == on) {
        return;
    }
    enabled_ = on;
    if(!on) {
        close_locked();
        last_sent_ = false;
        return;
    }
    apply_locked();
}

void SpiStatusLed::set_printing(bool on)
{
    std::lock_guard<std::mutex> lock(mtx_);
    printing_ = on;
    apply_locked();
}

void SpiStatusLed::set_debug_force(bool force, uint8_t color,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    std::lock_guard<std::mutex> lock(mtx_);
    const bool was_force = dbg_force_;
    dbg_force_ = force;
    if(force) {
        dbg_color_ = (color > static_cast<uint8_t>(Color::Raw))
                         ? Color::Blue
                         : static_cast<Color>(color);
        dbg_r_ = r;
        dbg_g_ = g;
        dbg_b_ = b;
    }
    if(force || enabled_) {
        /* force_send=true：同色重复点击也真发一帧，否则看不出灯板有没有响应；
         * 释放时同样强发，让 alarm/printing 推导结果立刻可见。 */
        apply_locked(true);
        return;
    }
    /* 释放接管且未进 V4：本模块本不该点灯，但验色已把灯点亮，而 WS 会锁存最后一帧。
     * 先刷回空闲蓝再关 fd，否则灯板一直停在测试色上。 */
    if(was_force) {
        send_rgb_locked(kBlueR, kBlueG, kBlueB);
    }
    close_locked();
    last_sent_ = false;
}

void SpiStatusLed::debug_force_state(bool& force, uint8_t& color,
                                     uint8_t& r, uint8_t& g, uint8_t& b)
{
    std::lock_guard<std::mutex> lock(mtx_);
    force = dbg_force_;
    color = static_cast<uint8_t>(dbg_color_);
    r = dbg_r_;
    g = dbg_g_;
    b = dbg_b_;
}

void SpiStatusLed::on_alarm_report(const serial_alarm_report_t& report)
{
    if((report.state_mask & SERIAL_ALARM_STATE_EVENT) != 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if((report.state_mask & SERIAL_ALARM_STATE_ACTIVE) != 0) {
        condition_active_.insert(report.alarm_code);
    } else {
        condition_active_.erase(report.alarm_code);
    }
    apply_locked();
}

void SpiStatusLed::on_alarm_snapshot(const serial_alarm_snapshot_t& snap)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(snap.nonzero_slot_count == 0) {
        condition_active_.clear();
        snap_expect_ = 0;
        snap_seen_ = 0;
        apply_locked();
        return;
    }
    if(snap.slot_id >= SERIAL_ALARM_SNAPSHOT_SLOTS) {
        return;
    }
    const uint16_t bit = static_cast<uint16_t>(1u << snap.slot_id);
    if(snap_expect_ != snap.nonzero_slot_count || (snap_seen_ & bit) != 0) {
        snap_expect_ = snap.nonzero_slot_count;
        snap_seen_ = 0;
        std::memset(snap_active_, 0, sizeof(snap_active_));
    }
    snap_active_[snap.slot_id] = snap.active_mask;
    snap_seen_ = static_cast<uint16_t>(snap_seen_ | bit);
    uint8_t got = 0;
    for(uint8_t i = 0; i < SERIAL_ALARM_SNAPSHOT_SLOTS; ++i) {
        if((snap_seen_ & static_cast<uint16_t>(1u << i)) != 0) {
            ++got;
        }
    }
    if(got >= snap_expect_) {
        commit_snapshot_round_locked();
    }
}

void SpiStatusLed::commit_snapshot_round_locked()
{
    condition_active_.clear();
    for(uint8_t slot = 0; slot < SERIAL_ALARM_SNAPSHOT_SLOTS; ++slot) {
        uint32_t m = snap_active_[slot];
        for(uint8_t b = 0; b < 32; ++b) {
            if((m & (1u << b)) != 0) {
                condition_active_.insert(snapshot_code_of(slot, b));
            }
        }
    }
    snap_expect_ = 0;
    snap_seen_ = 0;
    apply_locked();
}

bool SpiStatusLed::ensure_open_locked()
{
    if(fd_ >= 0) {
        return true;
    }
#if defined(__linux__)
    fd_ = ::open(GLAVO_SPI_STATUS_LED_DEV, O_RDWR);
    if(fd_ < 0) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi status led open %s failed errno=%d",
                                  GLAVO_SPI_STATUS_LED_DEV, errno);
        }
        return false;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t hz = kWsHz;
    if(::ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
       ::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
       ::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi status led ioctl setup failed errno=%d", errno);
        }
        close_locked();
        return false;
    }
    UPC_LOG_GLAVO_SERVICE(Info, "spi status led open %s", GLAVO_SPI_STATUS_LED_DEV);
    return true;
#else
    if(!open_warn_done_) {
        open_warn_done_ = true;
        UPC_LOG_GLAVO_SERVICE(Warn, "spi status led not supported on this host");
    }
    return false;
#endif
}

void SpiStatusLed::close_locked()
{
    if(fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void SpiStatusLed::apply_locked(bool force_send)
{
    /* 调试接管时绕开 MCU major≥4 版本门，否则台架上点按钮完全静默 */
    if(!enabled_ && !dbg_force_) {
        return;
    }
    Color c = Color::Blue;
    if(dbg_force_) {
        c = dbg_color_;
    } else if(!condition_active_.empty()) {
        c = Color::Red;
    } else if(printing_) {
        c = Color::Orange;
    }
    if(!force_send && last_sent_ && c == last_color_) {
        return;
    }
    uint8_t r = kBlueR, g = kBlueG, b = kBlueB;
    const char* name = "blue";
    if(c == Color::Raw) {
        r = dbg_r_;
        g = dbg_g_;
        b = dbg_b_;
        name = "raw";
    } else if(c == Color::Orange) {
        r = kOrangeR;
        g = kOrangeG;
        b = kOrangeB;
        name = "orange";
    } else if(c == Color::Red) {
        r = kRedR;
        g = kRedG;
        b = kRedB;
        name = "red";
    }
    if(!send_rgb_locked(r, g, b)) {
        return;
    }
    last_color_ = c;
    last_sent_ = true;
    UPC_LOG_GLAVO_SERVICE(Info, "spi status led color=%s rgb=%u,%u,%u%s", name,
                          static_cast<unsigned>(r), static_cast<unsigned>(g),
                          static_cast<unsigned>(b), dbg_force_ ? " (debug)" : "");
}

bool SpiStatusLed::send_rgb_locked(uint8_t r, uint8_t g, uint8_t b)
{
    if(!ensure_open_locked()) {
        return false;
    }
#if defined(__linux__)
    uint8_t buf[kBufSize];
    std::memset(buf, 0, sizeof(buf));
    for(uint8_t i = 0; i < kLedNum; ++i) {
        uint32_t p = static_cast<uint32_t>(i) * kBytesPerLed;
        enc_byte(buf, &p, g);
        enc_byte(buf, &p, r);
        enc_byte(buf, &p, b);
    }
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<unsigned long>(buf);
    tr.len = static_cast<uint32_t>(kBufSize);
    tr.speed_hz = kWsHz;
    tr.bits_per_word = 8;
    if(::ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi status led xfer failed errno=%d", errno);
        }
        close_locked();
        return false;
    }
    return true;
#else
    (void)r;
    (void)g;
    (void)b;
    return false;
#endif
}

} // namespace glavo
} // namespace upc

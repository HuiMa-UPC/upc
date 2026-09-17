#include "soc_sensors.hpp"
#include "device_config.hpp"

#include "debug/rs_log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#endif

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace upc {
namespace glavo {
namespace {

constexpr speed_t kPmBaud = B9600;

int open_uart_9600(const char* path)
{
    if(!path || !path[0]) {
        return -1;
    }
    const int fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) {
        return -1;
    }
    termios tio{};
    if(tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, kPmBaud);
    cfsetospeed(&tio, kPmBaud);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if(tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

bool write_sysfs_u8(const char* path, unsigned v)
{
    FILE* fp = std::fopen(path, "w");
    if(!fp) {
        return false;
    }
    const int n = std::fprintf(fp, "%u\n", v);
    const int rc = std::fclose(fp);
    return n > 0 && rc == 0;
}

bool read_text_int(const char* path, long* out)
{
    FILE* fp = std::fopen(path, "r");
    if(!fp) {
        return false;
    }
    long v = 0;
    const int n = std::fscanf(fp, "%ld", &v);
    std::fclose(fp);
    if(n != 1) {
        return false;
    }
    *out = v;
    return true;
}

uint16_t be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

/** PMSA003-A 主动帧：大气环境 PM1/2.5/10 → out；成功返回 true */
bool pmsa_try_parse_frame(const uint8_t* f, float* pm1, float* pm25, float* pm10)
{
    if(f[0] != 0x42 || f[1] != 0x4D) {
        return false;
    }
    const uint16_t flen = be16(f + 2);
    if(flen != 28) {
        return false;
    }
    uint16_t sum = 0;
    for(int i = 0; i < 30; ++i) {
        sum = static_cast<uint16_t>(sum + f[i]);
    }
    if(sum != be16(f + 30)) {
        return false;
    }
    if(f[29] != 0) {
        return false; /* error code */
    }
    /* Data4..6 大气环境（字节 10..15，0-based） */
    *pm1  = static_cast<float>(be16(f + 10));
    *pm25 = static_cast<float>(be16(f + 12));
    *pm10 = static_cast<float>(be16(f + 14));
    return true;
}

class PmUartReader {
public:
    explicit PmUartReader(const char* path) : path_(path ? path : "") {}

    void ensure_open()
    {
        if(fd_ >= 0) {
            return;
        }
        fd_ = open_uart_9600(path_.c_str());
    }

    void close_fd()
    {
        if(fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        buf_.clear();
    }

    /** 读串口并尝试解出最新一帧；有新帧返回 true */
    bool poll(float* pm1, float* pm25, float* pm10)
    {
        ensure_open();
        if(fd_ < 0) {
            return false;
        }
        uint8_t tmp[128];
        for(;;) {
            const ssize_t n = ::read(fd_, tmp, sizeof(tmp));
            if(n <= 0) {
                break;
            }
            buf_.insert(buf_.end(), tmp, tmp + n);
            if(buf_.size() > 512) {
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(buf_.size() - 256));
            }
        }
        bool got = false;
        while(buf_.size() >= 32) {
            size_t i = 0;
            while(i + 1 < buf_.size() && !(buf_[i] == 0x42 && buf_[i + 1] == 0x4D)) {
                ++i;
            }
            if(i > 0) {
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(i));
            }
            if(buf_.size() < 32) {
                break;
            }
            if(pmsa_try_parse_frame(buf_.data(), pm1, pm25, pm10)) {
                got = true;
                buf_.erase(buf_.begin(), buf_.begin() + 32);
            } else {
                buf_.erase(buf_.begin());
            }
        }
        return got;
    }

private:
    std::string path_;
    int fd_ = -1;
    std::vector<uint8_t> buf_;
};

bool find_aht20_hwmon(char* temp_path, size_t temp_sz, char* hum_path, size_t hum_sz)
{
    DIR* d = opendir("/sys/class/hwmon");
    if(!d) {
        return false;
    }
    bool ok = false;
    while(const dirent* ent = readdir(d)) {
        if(ent->d_name[0] == '.') {
            continue;
        }
        /* d_name ≤ NAME_MAX；路径缓冲按上限预留，截断则跳过 */
        char name_path[320];
        const int n_name = std::snprintf(
            name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", ent->d_name);
        if(n_name <= 0 || static_cast<size_t>(n_name) >= sizeof(name_path)) {
            continue;
        }
        FILE* fp = std::fopen(name_path, "r");
        if(!fp) {
            continue;
        }
        char name[64] = {};
        (void)std::fscanf(fp, "%63s", name);
        std::fclose(fp);
        if(std::strcmp(name, "aht20") != 0) {
            continue;
        }
        const int n_temp = std::snprintf(
            temp_path, temp_sz, "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        const int n_hum = std::snprintf(
            hum_path, hum_sz, "/sys/class/hwmon/%s/humidity1_input", ent->d_name);
        if(n_temp <= 0 || static_cast<size_t>(n_temp) >= temp_sz ||
           n_hum <= 0 || static_cast<size_t>(n_hum) >= hum_sz) {
            continue;
        }
        ok = true;
        break;
    }
    closedir(d);
    return ok;
}

bool read_aht20(float* temp_c, float* humidity)
{
    char tp[256], hp[256];
    if(!find_aht20_hwmon(tp, sizeof(tp), hp, sizeof(hp))) {
        return false;
    }
    long t_milli = 0, h_milli = 0;
    if(!read_text_int(tp, &t_milli) || !read_text_int(hp, &h_milli)) {
        return false;
    }
    *temp_c = static_cast<float>(t_milli) / 1000.f;
    *humidity = static_cast<float>(h_milli) / 1000.f;
    return true;
}

/** CPM121：/dev/i2c-N @0x6C，MR→延时→GD */
bool read_cpm121(const char* i2c_dev, float* kpa)
{
    const int fd = ::open(i2c_dev, O_RDWR);
    if(fd < 0) {
        return false;
    }
#if defined(__linux__)
    if(::ioctl(fd, I2C_SLAVE, 0x6c) != 0) {
        ::close(fd);
        return false;
    }
    uint8_t mr[2] = {0x30, 0x0a};
    if(::write(fd, mr, 2) != 2) {
        ::close(fd);
        return false;
    }
    usleep(6000);
    uint8_t reg = 0x06;
    uint8_t buf[5] = {};
    struct i2c_msg msgs[2] = {};
    msgs[0].addr = 0x6c;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = 0x6c;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = 5;
    msgs[1].buf = buf;
    struct i2c_rdwr_ioctl_data d = {};
    d.msgs = msgs;
    d.nmsgs = 2;
    if(::ioctl(fd, I2C_RDWR, &d) < 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    const uint32_t raw = (static_cast<uint32_t>(buf[0]) << 16) |
                         (static_cast<uint32_t>(buf[1]) << 8) |
                         static_cast<uint32_t>(buf[2]);
    *kpa = static_cast<float>(raw) / 64.f / 1000.f;
    return true;
#else
    (void)i2c_dev;
    (void)kpa;
    ::close(fd);
    return false;
#endif
}

/**
 * IIO raw → 氧浓度 %。
 * V = raw / ADC_MAX * 3.3；I[uA] = V / R_sense × 1e6；O2 = 100*(1-exp(-I/K))。
 */
bool read_o2_percent(float* oxygen_percent)
{
    long raw = 0;
    if(!read_text_int(GLAVO_O2_IIO_RAW, &raw) || raw < 0) {
        return false;
    }
    const float v = (static_cast<float>(raw) / static_cast<float>(GLAVO_O2_ADC_MAX)) *
                    GLAVO_O2_VREF;
    if(!(v >= 0.f) || !std::isfinite(v)) {
        return false;
    }
    const float i_ua = (v / GLAVO_O2_RSENSE_OHM) * 1.0e6f;
    const float k = GLAVO_O2_K;
    if(!(k > 0.f)) {
        return false;
    }
    float o2 = 100.f * (1.f - std::exp(-i_ua / k));
    if(!std::isfinite(o2)) {
        return false;
    }
    if(o2 < 0.f) {
        o2 = 0.f;
    } else if(o2 > 100.f) {
        o2 = 100.f;
    }
    *oxygen_percent = o2;
    return true;
}

} // namespace

SocSensors& SocSensors::instance()
{
    static SocSensors inst;
    return inst;
}

void SocSensors::start()
{
    bool expected = false;
    if(!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    worker_ = std::thread([this]() { thread_fn(); });
}

void SocSensors::stop()
{
    running_.store(false);
    if(worker_.joinable()) {
        worker_.join();
    }
    set_pm_valve(false);
}

bool SocSensors::set_v4_mode(bool enabled)
{
    const bool was = v4_mode_.exchange(enabled);
    if(enabled && !was) {
        const bool ok = set_all_cabin_leds(true);
        UPC_LOG_GLAVO_SERVICE(Info, "V4: cabin LEDs all on ok=%d mask=0x%02X",
                              ok ? 1 : 0,
                              static_cast<unsigned>(cabin_led_mask()));
        return true;
    }
    return false;
}

SocEnvSample SocSensors::snapshot() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return cache_;
}

bool SocSensors::pm_valve_open() const noexcept
{
    return valve_open_.load();
}

void SocSensors::apply_pm_valve_locked(bool open)
{
    if(write_sysfs_u8(GLAVO_PM_VALVE_SYSFS, open ? 1u : 0u)) {
        valve_open_.store(open);
    }
}

void SocSensors::set_pm_valve(bool open)
{
    std::lock_guard<std::mutex> lock(valve_mtx_);
    debug_valve_want_.store(open);
    apply_pm_valve_locked(open);
}

namespace {

const char* cabin_led_sysfs(uint8_t channel)
{
    switch(channel) {
        case 0: return GLAVO_SOC_LED_LGT1L_SYSFS;
        case 1: return GLAVO_SOC_LED_LGT1R_SYSFS;
        case 2: return GLAVO_SOC_LED_LGT2L_SYSFS;
        case 3: return GLAVO_SOC_LED_LGT2R_SYSFS;
        case 4: return GLAVO_SOC_LED_LGT3L_SYSFS;
        case 5: return GLAVO_SOC_LED_LGT3R_SYSFS;
        default: return nullptr;
    }
}

} // namespace

bool SocSensors::set_cabin_led(uint8_t channel, bool on)
{
    const char* path = cabin_led_sysfs(channel);
    if(!path) {
        return false;
    }
    if(!write_sysfs_u8(path, on ? 1u : 0u)) {
        return false;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << channel);
    if(on) {
        led_mask_.fetch_or(bit);
    } else {
        led_mask_.fetch_and(static_cast<uint8_t>(0xFFu ^ bit));
    }
    return true;
}

bool SocSensors::cabin_led_on(uint8_t channel) const
{
    if(channel >= kLedCount) {
        return false;
    }
    const char* path = cabin_led_sysfs(channel);
    long v = 0;
    if(path && read_text_int(path, &v)) {
        return v != 0;
    }
    return (led_mask_.load() & static_cast<uint8_t>(1u << channel)) != 0;
}

uint8_t SocSensors::cabin_led_mask() const
{
    uint8_t mask = 0;
    for(uint8_t i = 0; i < kLedCount; ++i) {
        if(cabin_led_on(i)) {
            mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(1u << i));
        }
    }
    return mask;
}

bool SocSensors::set_all_cabin_leds(bool on)
{
    bool all_ok = true;
    for(uint8_t i = 0; i < kLedCount; ++i) {
        if(!set_cabin_led(i, on)) {
            all_ok = false;
        }
    }
    return all_ok;
}

float SocSensors::sample_cabin_pm25_avg(uint32_t sample_ms)
{
    if(sample_ms == 0) {
        sample_ms = 3000;
    }
    std::lock_guard<std::mutex> sample_lock(sample_mtx_);
    cabin_accum_sum_.store(0.0);
    cabin_accum_count_.store(0);
    cabin_accum_active_.store(true);
    {
        std::lock_guard<std::mutex> lock(valve_mtx_);
        apply_pm_valve_locked(true);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(sample_ms);
    while(std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cabin_accum_active_.store(false);
    {
        std::lock_guard<std::mutex> lock(valve_mtx_);
        /* 采样窗口结束必须关阀；同步清调试意愿，避免按钮仍显示 On */
        debug_valve_want_.store(false);
        apply_pm_valve_locked(false);
    }

    const int n = cabin_accum_count_.load();
    if(n <= 0) {
        /* 回退：用缓存里最后一帧舱内值（阀关后可能无效） */
        const auto snap = snapshot();
        return (snap.pm25_cabin >= 0.f) ? snap.pm25_cabin : -1.f;
    }
    return static_cast<float>(cabin_accum_sum_.load() / static_cast<double>(n));
}

void SocSensors::poll_once()
{
    static PmUartReader outdoor(GLAVO_PM_OUTDOOR_UART);
    static PmUartReader cabin(GLAVO_PM_CABIN_UART);

    SocEnvSample s;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        s = cache_;
    }

    float pm1 = 0, pm25 = 0, pm10 = 0;
    if(outdoor.poll(&pm1, &pm25, &pm10)) {
        s.pm1_outdoor = pm1;
        s.pm25_outdoor = pm25;
        s.pm10_outdoor = pm10;
        s.valid = true;
    }
    if(cabin.poll(&pm1, &pm25, &pm10)) {
        s.pm1_cabin = pm1;
        s.pm25_cabin = pm25;
        s.pm10_cabin = pm10;
        s.valid = true;
        if(cabin_accum_active_.load() && valve_open_.load()) {
            cabin_accum_sum_.store(cabin_accum_sum_.load() + static_cast<double>(pm25));
            cabin_accum_count_.fetch_add(1);
        }
    }

    float t = 0, h = 0;
    if(read_aht20(&t, &h)) {
        s.temperature_c = t;
        s.humidity_percent = h;
        s.valid = true;
    }

    float p = 0;
    if(read_cpm121("/dev/i2c-11", &p)) {
        s.pressure1_kpa = p;
        s.valid = true;
    }
    if(read_cpm121("/dev/i2c-12", &p)) {
        s.pressure2_kpa = p;
        s.valid = true;
    }

    float o2 = 0.f;
    if(read_o2_percent(&o2)) {
        s.oxygen_percent = o2;
        s.valid = true;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        cache_ = s;
    }
}

void SocSensors::thread_fn()
{
    UPC_LOG_GLAVO_SERVICE(Info, "SocSensors thread started");
    while(running_.load()) {
        if(v4_mode_.load()) {
            poll_once();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    UPC_LOG_GLAVO_SERVICE(Info, "SocSensors thread stopped");
}

} // namespace glavo
} // namespace upc

#include "soc_sensors.hpp"
#include "device_config.hpp"

#include "debug/rs_log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
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
 * IIO raw → 电压 / 电流。
 * 本板无 in_voltage0_scale：raw 单位 nV，V = raw × GLAVO_O2_RAW_TO_VOLT；
 * I[uA] = V / R_sense × 1e6。
 */
bool read_o2_raw_i(long* raw_out, float* i_ua_out)
{
    long raw = 0;
    if(!read_text_int(GLAVO_O2_IIO_RAW, &raw) || raw < 0) {
        return false;
    }
    const float v = static_cast<float>(raw) * GLAVO_O2_RAW_TO_VOLT;
    if(!(v >= 0.f) || !std::isfinite(v)) {
        return false;
    }
    const float i_ua = (v / GLAVO_O2_RSENSE_OHM) * 1.0e6f;
    if(!std::isfinite(i_ua) || i_ua < 0.f) {
        return false;
    }
    if(raw_out) {
        *raw_out = raw;
    }
    if(i_ua_out) {
        *i_ua_out = i_ua;
    }
    return true;
}

/** I[uA] + K → 氧浓度 %。 */
bool o2_from_i(float i_ua, float k, float* oxygen_percent)
{
    if(!(k > 0.f) || !std::isfinite(i_ua) || i_ua < 0.f) {
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

bool read_o2_percent(float k, float* oxygen_percent)
{
    float i_ua = 0.f;
    if(!read_o2_raw_i(nullptr, &i_ua)) {
        return false;
    }
    return o2_from_i(i_ua, k, oxygen_percent);
}

/** 从简易 JSON 文本里取 "key": number。 */
bool json_find_number(const char* text, const char* key, float* out)
{
    if(!text || !key || !out) {
        return false;
    }
    char pat[64];
    std::snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = std::strstr(text, pat);
    if(!p) {
        return false;
    }
    p = std::strchr(p + std::strlen(pat), ':');
    if(!p) {
        return false;
    }
    ++p;
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    char* end = nullptr;
    const float v = std::strtof(p, &end);
    if(end == p || !std::isfinite(v)) {
        return false;
    }
    *out = v;
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
    load_o2_cal_from_json();
    worker_ = std::thread([this]() { thread_fn(); });
}

void SocSensors::load_o2_cal_from_json()
{
    FILE* fp = std::fopen(GLAVO_O2_CAL_JSON_PATH, "r");
    if(!fp) {
        UPC_LOG_GLAVO_SERVICE(Info, "o2 cal: no %s, use default K=%.1f",
                              GLAVO_O2_CAL_JSON_PATH,
                              static_cast<double>(GLAVO_O2_K));
        o2_k_.store(GLAVO_O2_K);
        o2_cal_ref_.store(GLAVO_O2_CAL_REF_PERCENT);
        return;
    }
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    buf[n] = '\0';

    float k = 0.f;
    float ref = GLAVO_O2_CAL_REF_PERCENT;
    if(!json_find_number(buf, "k", &k) ||
       !(k >= GLAVO_O2_CAL_K_MIN && k <= GLAVO_O2_CAL_K_MAX)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "o2 cal: invalid k in %s, keep default %.1f",
                              GLAVO_O2_CAL_JSON_PATH,
                              static_cast<double>(GLAVO_O2_K));
        o2_k_.store(GLAVO_O2_K);
        o2_cal_ref_.store(GLAVO_O2_CAL_REF_PERCENT);
        return;
    }
    float ref_tmp = 0.f;
    if(json_find_number(buf, "ref_o2_percent", &ref_tmp) &&
       ref_tmp > 0.f && ref_tmp < 100.f) {
        ref = ref_tmp;
    }
    o2_k_.store(k);
    o2_cal_ref_.store(ref);
    UPC_LOG_GLAVO_SERVICE(Info, "o2 cal: loaded K=%.3f ref=%.2f%% from %s",
                          static_cast<double>(k), static_cast<double>(ref),
                          GLAVO_O2_CAL_JSON_PATH);
}

bool SocSensors::save_o2_cal_json(float k, float ref_o2, float i_ua, long raw_nv,
                                  uint32_t sample_count)
{
    /* 确保 /userdata/CAL 存在 */
    (void)::mkdir("/userdata", 0755);
    (void)::mkdir("/userdata/CAL", 0755);

    char tmp_path[256];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", GLAVO_O2_CAL_JSON_PATH);
    FILE* fp = std::fopen(tmp_path, "w");
    if(!fp) {
        UPC_LOG_GLAVO_SERVICE(Warn, "o2 cal: open %s failed errno=%d",
                              tmp_path, errno);
        return false;
    }
    const std::time_t now = std::time(nullptr);
    const int n = std::fprintf(
        fp,
        "{\n"
        "  \"version\": 1,\n"
        "  \"k\": %.6f,\n"
        "  \"ref_o2_percent\": %.3f,\n"
        "  \"i_ua\": %.3f,\n"
        "  \"raw_nv\": %ld,\n"
        "  \"rsense_ohm\": %.1f,\n"
        "  \"sample_count\": %u,\n"
        "  \"calibrated_unix\": %lld,\n"
        "  \"source\": \"user\"\n"
        "}\n",
        static_cast<double>(k),
        static_cast<double>(ref_o2),
        static_cast<double>(i_ua),
        raw_nv,
        static_cast<double>(GLAVO_O2_RSENSE_OHM),
        sample_count,
        static_cast<long long>(now));
    if(std::fflush(fp) != 0 || n <= 0) {
        std::fclose(fp);
        (void)::unlink(tmp_path);
        return false;
    }
    std::fclose(fp);
    if(::rename(tmp_path, GLAVO_O2_CAL_JSON_PATH) != 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "o2 cal: rename to %s failed errno=%d",
                              GLAVO_O2_CAL_JSON_PATH, errno);
        (void)::unlink(tmp_path);
        return false;
    }
    return true;
}

void SocSensors::fill_o2_calib_state(middleware::DebugO2CalibRspPayload* out) const
{
    if(!out) {
        return;
    }
    *out = {};
    out->ok = 1;
    out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::Ok);
    out->k = o2_k_.load();
    out->ref_o2_percent = o2_cal_ref_.load();
    out->i_ua = 0.f;
    out->o2_percent = -1.f;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(cache_.oxygen_percent >= 0.f) {
            out->o2_percent = cache_.oxygen_percent;
        }
    }
}

bool SocSensors::start_calibrate_o2(
    float ref_o2_percent,
    std::function<void(const middleware::DebugO2CalibRspPayload&)> done)
{
    bool expected = false;
    if(!o2_calib_busy_.compare_exchange_strong(expected, true)) {
        return false;
    }
    o2_calib_cancel_.store(false);

    std::lock_guard<std::mutex> lock(o2_calib_thread_mtx_);
    if(o2_calib_thread_.joinable()) {
        o2_calib_thread_.join();
    }
    try {
        o2_calib_thread_ = std::thread(
            [this, ref_o2_percent, done = std::move(done)]() {
                middleware::DebugO2CalibRspPayload out{};
                run_o2_calib(ref_o2_percent, &out);
                if(done) {
                    done(out);
                }
            });
    } catch(...) {
        o2_calib_busy_.store(false);
        return false;
    }
    return true;
}

void SocSensors::join_o2_calib()
{
    o2_calib_cancel_.store(true);
    std::lock_guard<std::mutex> lock(o2_calib_thread_mtx_);
    if(o2_calib_thread_.joinable()) {
        o2_calib_thread_.join();
    }
    o2_calib_cancel_.store(false);
}

void SocSensors::run_o2_calib(float ref_o2_percent,
                              middleware::DebugO2CalibRspPayload* out)
{
    middleware::DebugO2CalibRspPayload local{};
    if(!out) {
        out = &local;
    }
    fill_o2_calib_state(out);
    out->ok = 0;

    struct BusyGuard {
        std::atomic<bool>* flag;
        ~BusyGuard() { flag->store(false); }
    } guard{&o2_calib_busy_};

    float ref = ref_o2_percent;
    if(!(ref > 0.f)) {
        ref = GLAVO_O2_CAL_REF_PERCENT;
    }
    if(!(ref > 0.f && ref < 100.f)) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::BadRef);
        return;
    }
    out->ref_o2_percent = ref;

    if(::access(GLAVO_O2_READY_PATH, F_OK) != 0) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::NotReady);
        return;
    }

    const uint32_t n_samp = GLAVO_O2_CAL_SAMPLE_COUNT;
    const uint32_t n_min =
        std::min<uint32_t>(GLAVO_O2_CAL_SAMPLE_MIN, n_samp);
    double sum_i = 0.0;
    double sum_raw = 0.0;
    uint32_t ok_n = 0;
    for(uint32_t i = 0; i < n_samp; ++i) {
        if(o2_calib_cancel_.load()) {
            out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::IioFail);
            UPC_LOG_GLAVO_SERVICE(Warn, "o2 cal: cancelled");
            return;
        }
        long raw = 0;
        float i_ua = 0.f;
        if(read_o2_raw_i(&raw, &i_ua)) {
            sum_i += static_cast<double>(i_ua);
            sum_raw += static_cast<double>(raw);
            ++ok_n;
        }
        if(i + 1 < n_samp) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(GLAVO_O2_CAL_SAMPLE_INTERVAL_MS));
        }
    }
    if(ok_n < n_min) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::IioFail);
        UPC_LOG_GLAVO_SERVICE(Warn, "o2 cal: samples %u < min %u", ok_n, n_min);
        return;
    }

    const float i_avg = static_cast<float>(sum_i / static_cast<double>(ok_n));
    const long raw_avg =
        static_cast<long>(sum_raw / static_cast<double>(ok_n) + 0.5);
    out->i_ua = i_avg;

    if(!(i_avg >= GLAVO_O2_CAL_I_MIN_UA && i_avg <= GLAVO_O2_CAL_I_MAX_UA)) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::IOutOfRange);
        UPC_LOG_GLAVO_SERVICE(Warn,
            "o2 cal: I=%.3fuA out of range [%.2f,%.0f]",
            static_cast<double>(i_avg),
            static_cast<double>(GLAVO_O2_CAL_I_MIN_UA),
            static_cast<double>(GLAVO_O2_CAL_I_MAX_UA));
        return;
    }

    /* k = -I / ln(1 - ref/100) */
    const float one_minus = 1.f - ref / 100.f;
    if(!(one_minus > 0.f && one_minus < 1.f)) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::BadRef);
        return;
    }
    const float k = -i_avg / std::log(one_minus);
    if(!(k >= GLAVO_O2_CAL_K_MIN && k <= GLAVO_O2_CAL_K_MAX) ||
       !std::isfinite(k)) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::KOutOfRange);
        UPC_LOG_GLAVO_SERVICE(Warn, "o2 cal: K=%.3f out of range",
                              static_cast<double>(k));
        return;
    }

    if(!save_o2_cal_json(k, ref, i_avg, raw_avg, ok_n)) {
        out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::WriteFail);
        return;
    }

    o2_k_.store(k);
    o2_cal_ref_.store(ref);
    float o2 = 0.f;
    if(o2_from_i(i_avg, k, &o2)) {
        out->o2_percent = o2;
        std::lock_guard<std::mutex> lock(mtx_);
        cache_.oxygen_percent = o2;
        cache_.valid = true;
    }
    out->k = k;
    out->ok = 1;
    out->err = static_cast<uint8_t>(middleware::DebugO2CalibErr::Ok);
    UPC_LOG_GLAVO_SERVICE(Info,
        "o2 cal: OK K=%.3f I=%.2fuA ref=%.2f%% O2=%.2f%% samples=%u raw=%ld",
        static_cast<double>(k), static_cast<double>(i_avg),
        static_cast<double>(ref), static_cast<double>(out->o2_percent),
        ok_n, raw_avg);
}

void SocSensors::stop()
{
    running_.store(false);
    if(worker_.joinable()) {
        worker_.join();
    }
    join_o2_calib();
    set_pm_valve(false);
}

bool SocSensors::set_v4_mode(bool enabled)
{
    const bool was = v4_mode_.exchange(enabled);
    if(!enabled) {
        /* 退出 V4 后允许再次确认版本时重做默认全开 */
        cabin_leds_default_applied_.store(false);
    }
    return enabled && !was;
}

bool SocSensors::ensure_cabin_leds_default_on()
{
    bool expected = false;
    if(!cabin_leds_default_applied_.compare_exchange_strong(expected, true)) {
        return false;
    }
    const bool ok = set_all_cabin_leds(true);
    UPC_LOG_GLAVO_SERVICE(Info, "V4: cabin LEDs all on (MCU≥4) ok=%d mask=0x%02X",
                          ok ? 1 : 0,
                          static_cast<unsigned>(cabin_led_mask()));
    return true;
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

    /* AHT20：最短 2s 一采；间隔内沿用 cache_ 里上一拍温湿度 */
    static auto last_aht20 = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    const bool aht20_due = (last_aht20.time_since_epoch().count() == 0) ||
        (now - last_aht20 >= std::chrono::milliseconds(GLAVO_AHT20_MIN_INTERVAL_MS));
    if(aht20_due) {
        float t = 0, h = 0;
        if(read_aht20(&t, &h)) {
            s.temperature_c = t;
            s.humidity_percent = h;
            s.valid = true;
            last_aht20 = now;
        }
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

    /* 氧气：预热完成标志 /run/o2_ready 出现后才读 IIO；PM 圈仍 200ms */
    static bool o2_ready = false;
    if(!o2_ready) {
        o2_ready = (::access(GLAVO_O2_READY_PATH, F_OK) == 0);
    }
    if(o2_ready) {
        float o2 = 0.f;
        if(read_o2_percent(o2_k_.load(), &o2)) {
            s.oxygen_percent = o2;
            s.valid = true;
        }
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

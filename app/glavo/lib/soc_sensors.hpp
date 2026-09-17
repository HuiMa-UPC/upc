#ifndef SOC_SENSORS_HPP
#define SOC_SENSORS_HPP

/**
 * @file soc_sensors.hpp
 * @brief SOC 侧外设：舱内外 PMSA / AHT20 / CPM121 / IIO 氧气 周期采集（MCU≥V4 才轮询）；
 *        六路舱灯与 fa_valve 为 sysfs 直控，**不看 MCU 版本**（调试页可随时开关）。
 *
 * 背光调节在 UI 进程，不经本模块、也不看 MCU 版本。
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace upc {
namespace glavo {

struct SocEnvSample {
    float pm25_outdoor = -1.f;  /**< 舱外（常显） */
    float pm1_outdoor  = -1.f;
    float pm10_outdoor = -1.f;
    float pm25_cabin   = -1.f;  /**< 舱内（仅阀开采样窗口可信） */
    float pm1_cabin    = -1.f;
    float pm10_cabin   = -1.f;
    float temperature_c = -1.f;
    float humidity_percent = -1.f;
    float pressure1_kpa = -1.f; /**< SC1 */
    float pressure2_kpa = -1.f; /**< SC2 */
    float oxygen_percent = -1.f; /**< IIO ADC + 电流公式；无效 <0 */
    bool  valid = false;
};

/**
 * @brief 后台线程轮询 SOC 侧传感器（V4）；LED / fa_valve 随时可写。
 */
class SocSensors {
public:
    static constexpr uint8_t kLedCount = 6;

    static SocSensors& instance();

    void start();
    void stop();

    /** MCU≥V4 时打开周期采集；unknown/&lt;V4 关闭（不清已采缓存）。
     *  关模式不清阀/灯：调试页可随时开关，不随版本门控。
     *  @return true=本次从非 V4 切入 V4（已点亮六路舱灯） */
    bool set_v4_mode(bool enabled);

    [[nodiscard]] SocEnvSample snapshot() const;

    /** 舱内 PM 电磁阀（fa_valve）；不看 MCU 版本。立即写 sysfs。 */
    void set_pm_valve(bool open);

    [[nodiscard]] bool pm_valve_open() const noexcept;

    /**
     * @brief 开阀采样舱内 PM，阀开 sample_ms 后**强制关阀**，返回窗口内平均 pm2.5。
     * 与其它 sample / set_pm_valve 互斥（采样窗口串行）。旧板无阀时勿调用。
     * @return ≥0 平均值；&lt;0 失败
     */
    float sample_cabin_pm25_avg(uint32_t sample_ms);

    /**
     * @brief 六路舱灯单独开关（lgt1l…lgt3r）；channel 0..5；不看 MCU 版本。
     * @return false=channel 非法或 sysfs 写失败
     */
    bool set_cabin_led(uint8_t channel, bool on);

    [[nodiscard]] bool cabin_led_on(uint8_t channel) const;

    /** bit0=lgt1l … bit5=lgt3r；优先读 sysfs，失败回退内存态 */
    [[nodiscard]] uint8_t cabin_led_mask() const;

    /** 六路舱灯一齐开关；不看 MCU 版本。返回全部 sysfs 写入成功 */
    bool set_all_cabin_leds(bool on);

private:
    SocSensors() = default;
    void thread_fn();
    void poll_once();

    std::atomic<bool> running_{false};
    std::atomic<bool> v4_mode_{false};
    std::atomic<bool> valve_open_{false};
    std::atomic<bool> debug_valve_want_{false};
    std::atomic<uint8_t> led_mask_{0};
    std::thread worker_;
    std::mutex sample_mtx_;
    std::mutex valve_mtx_;

    void apply_pm_valve_locked(bool open);

    mutable std::mutex mtx_;
    SocEnvSample cache_{};
    /* 舱内采样窗口累加（仅 sample_cabin_pm25_avg 期间使用） */
    std::atomic<bool> cabin_accum_active_{false};
    std::atomic<double> cabin_accum_sum_{0.0};
    std::atomic<int> cabin_accum_count_{0};
};

} // namespace glavo
} // namespace upc

#endif

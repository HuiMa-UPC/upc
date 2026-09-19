#ifndef SOC_SENSORS_HPP
#define SOC_SENSORS_HPP

/**
 * @file soc_sensors.hpp
 * @brief SOC 侧外设：舱内外 PMSA / AHT20（≥2s）/ CPM121 / IIO 氧气（须 /run/o2_ready）周期采集（MCU≥V4 才轮询）；
 *        六路舱灯与 fa_valve 为 sysfs 直控，**不看 MCU 版本**（调试页可随时开关）。
 *
 * 背光调节在 UI 进程，不经本模块、也不看 MCU 版本。
 */

#include "device_config.hpp"
#include "upc_contract.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
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

    /** MCU≥V4 / Len=15 时打开周期采集；unknown/&lt;V4 关闭（不清已采缓存）。
     *  关模式不清阀/灯：调试页可随时开关，不随版本门控。
     *  舱灯默认全开不在此函数——须等固件 major≥4 后调 ensure_cabin_leds_default_on()。
     *  @return true=本次从非 V4 切入 V4 */
    bool set_v4_mode(bool enabled);

    /** 仅 MCU 固件 major≥4 确认后调用：首次把六路舱灯全开。
     *  @return true=本次执行了全开（可发 LED Ntf） */
    bool ensure_cabin_leds_default_on();

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

    /**
     * @brief 空气/标准气单点标定：独立线程多采 IIO → 求 K → 原子写 JSON → 内存立即生效。
     * @param ref_o2_percent 参考浓度 %；≤0 用 GLAVO_O2_CAL_REF_PERCENT
     * @param done 完成后回调（任意线程；Glavo 侧须 publish_async）
     * @return false=已有标定在跑（调用方填 Busy，不会调 done）
     * @note 不阻塞调用方；stop()/join_o2_calib() 会等线程结束。
     */
    bool start_calibrate_o2(
        float ref_o2_percent,
        std::function<void(const middleware::DebugO2CalibRspPayload&)> done);

    /** 取消进行中的采样并 join 标定线程（GlavoService::stop 须先于销毁回调） */
    void join_o2_calib();

    /** 仅回读当前 K/氧（不采样标定）；填 out.ok=1 */
    void fill_o2_calib_state(middleware::DebugO2CalibRspPayload* out) const;

private:
    SocSensors() = default;
    void thread_fn();
    void poll_once();
    void load_o2_cal_from_json();
    bool save_o2_cal_json(float k, float ref_o2, float i_ua, long raw_nv,
                          uint32_t sample_count);
    void run_o2_calib(float ref_o2_percent, middleware::DebugO2CalibRspPayload* out);

    std::atomic<bool> running_{false};
    std::atomic<bool> v4_mode_{false};
    std::atomic<bool> cabin_leds_default_applied_{false};
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

    std::atomic<float> o2_k_{GLAVO_O2_K};
    std::atomic<float> o2_cal_ref_{GLAVO_O2_CAL_REF_PERCENT};
    std::atomic<bool> o2_calib_busy_{false};
    std::atomic<bool> o2_calib_cancel_{false};
    std::mutex o2_calib_thread_mtx_;
    std::thread o2_calib_thread_;
};

} // namespace glavo
} // namespace upc

#endif

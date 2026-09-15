#ifndef UPC_PLATE_HEIGHT_MEASURE_HPP
#define UPC_PLATE_HEIGHT_MEASURE_HPP

/**
 * @file plate_height_measure.hpp
 * @brief 打印前两步测高：粗 LUT → AXIS_INC → 精测 30μm/200μm 门。
 *
 * start() 起独立线程；结果经 on_done 回灌 worker。失败不阻断打印。
 */

#include "galvo_motion_encode.hpp"
#include "height_lut_2d.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace upc::glavo {

struct PlateHeightMeasureResult {
    bool   ran = false;      /**< true=跑过测高（含软失败仍调了 Z） */
    bool   ok = false;       /**< true=粗测有效并完成调 Z */
    bool   flatness_warn = false;
    float  h_med_mm = 0.0f;
};

class PlateHeightMeasure {
public:
    using SendMotionFn = std::function<bool(const uint8_t* subpacket)>;
    using PrepareIlluminationFn = std::function<bool()>;
    using SendZHomeFn = std::function<void()>;
    using SendZIncFn = std::function<void(float delta_mm)>;
    using WaitZFn = std::function<bool(float target_mm, float tol_mm, int timeout_ms)>;
    using RequestSpotFn = std::function<bool(float& x, float& y, int timeout_ms)>;
    using PublishSessionFn = std::function<void(bool active)>;
    using DoneFn = std::function<void(const PlateHeightMeasureResult&)>;

    PlateHeightMeasure() = default;
    ~PlateHeightMeasure();

    PlateHeightMeasure(const PlateHeightMeasure&) = delete;
    PlateHeightMeasure& operator=(const PlateHeightMeasure&) = delete;

    void configure(SendMotionFn send_motion,
                   PrepareIlluminationFn prepare_illumination,
                   SendZHomeFn send_z_home,
                   SendZIncFn send_z_inc,
                   WaitZFn wait_z,
                   RequestSpotFn request_spot,
                   PublishSessionFn publish_session,
                   DoneFn on_done);

    /**
     * @brief 起一轮测高。调用前须已确认 LUT 可读。
     * @return false=已有一轮在跑或未配置
     */
    bool start(const GalvoMotionCalibration& cal);

    void abort();
    [[nodiscard]] bool active() const noexcept { return active_.load(); }
    [[nodiscard]] bool cancel_requested() const noexcept {
        return abort_requested_.load();
    }

private:
    struct Scan {
        float h_med = 0.0f;
        float max_dev = 0.0f;
        int n = 0;
    };

    void run(GalvoMotionCalibration cal);
    Scan scan(const HeightLut2d& lut, const GalvoMotionCalibration& cal,
              const char* tag);

    SendMotionFn send_motion_;
    PrepareIlluminationFn prepare_illumination_;
    SendZHomeFn send_z_home_;
    SendZIncFn send_z_inc_;
    WaitZFn wait_z_;
    RequestSpotFn request_spot_;
    PublishSessionFn publish_session_;
    DoneFn on_done_;

    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<uint32_t> generation_{0};
};

} // namespace upc::glavo

#endif

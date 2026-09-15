#ifndef UPC_HEIGHT_CALIBRATION_HPP
#define UPC_HEIGHT_CALIBRATION_HPP

/**
 * @file height_calibration.hpp
 * @brief 调试页激光高度校准：关舱灯→开红光→Z 网格采集→写 5 列 LUT。
 *
 * start() 在独立线程跑整轮；同步等 Visual 质心必须在该线程，不能占 Glavo worker。
 */

#include "galvo_motion_encode.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace upc::glavo {

class HeightCalibration {
public:
    using SendMotionFn = std::function<bool(const uint8_t* subpacket)>;
    using PrepareIlluminationFn = std::function<bool()>;
    using SendZHomeFn = std::function<void()>;
    using SendZIncFn = std::function<void(float delta_mm)>;
    using WaitZFn = std::function<bool(float target_mm, float tol_mm, int timeout_ms)>;
    using RequestSpotFn = std::function<bool(float& x, float& y, int timeout_ms)>;
    using PublishProgressFn = std::function<void(uint8_t stage, float height_mm,
                                                 uint16_t collected, uint16_t total)>;
    using TurnRedOffFn = std::function<void()>;

    HeightCalibration() = default;
    ~HeightCalibration();

    HeightCalibration(const HeightCalibration&) = delete;
    HeightCalibration& operator=(const HeightCalibration&) = delete;

    void configure(SendMotionFn send_motion,
                   PrepareIlluminationFn prepare_illumination,
                   SendZHomeFn send_z_home,
                   SendZIncFn send_z_inc,
                   WaitZFn wait_z,
                   RequestSpotFn request_spot,
                   PublishProgressFn publish_progress,
                   TurnRedOffFn turn_red_off);

    bool start(const GalvoMotionCalibration& cal,
               float start_mm, float end_mm, float step_mm,
               std::string lut_path);

    void abort();
    [[nodiscard]] bool active() const noexcept { return active_.load(); }
    [[nodiscard]] bool cancel_requested() const noexcept {
        return abort_requested_.load();
    }

private:
    void run(GalvoMotionCalibration cal, float start_mm, float end_mm,
             float step_mm, std::string lut_path);

    SendMotionFn send_motion_;
    PrepareIlluminationFn prepare_illumination_;
    SendZHomeFn send_z_home_;
    SendZIncFn send_z_inc_;
    WaitZFn wait_z_;
    RequestSpotFn request_spot_;
    PublishProgressFn publish_progress_;
    TurnRedOffFn turn_red_off_;

    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> abort_requested_{false};
};

} // namespace upc::glavo

#endif

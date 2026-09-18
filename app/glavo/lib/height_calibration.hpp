#ifndef UPC_HEIGHT_CALIBRATION_HPP
#define UPC_HEIGHT_CALIBRATION_HPP

/**
 * @file height_calibration.hpp
 * @brief 调试页高度校准：关舱灯+Red_On → 回零（先下降 20mm 再 Home）→ Z 网格采点 → 写 LUT → 开舱灯（红光保持）。
 */

#include "galvo_motion_encode.hpp"
#include "height_points.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace upc::glavo {

class HeightCalibration {
public:
    using PublishProgressFn = std::function<void(uint8_t stage, float height_mm,
                                                 uint16_t collected, uint16_t total)>;

    HeightCalibration() = default;
    ~HeightCalibration();

    HeightCalibration(const HeightCalibration&) = delete;
    HeightCalibration& operator=(const HeightCalibration&) = delete;

    void configure(HeightMoveGalvoFn move_galvo,
                   HeightSessionLightFn set_session_light,
                   HeightSendZHomeFn send_z_home,
                   HeightSendZIncFn send_z_inc,
                   HeightWaitZFn wait_z,
                   HeightRequestSpotFn request_spot,
                   PublishProgressFn publish_progress);

    bool start(const GalvoMotionCalibration& cal,
               float start_mm, float end_mm, float step_mm,
               std::string lut_path);

    void abort();
    [[nodiscard]] bool active() const noexcept { return active_.load(); }
    [[nodiscard]] bool cancel_requested() const noexcept {
        return abort_requested_.load();
    }

private:
    void run(float start_mm, float end_mm, float step_mm, std::string lut_path);

    HeightMoveGalvoFn move_galvo_;
    HeightSessionLightFn set_session_light_;
    HeightSendZHomeFn send_z_home_;
    HeightSendZIncFn send_z_inc_;
    HeightWaitZFn wait_z_;
    HeightRequestSpotFn request_spot_;
    PublishProgressFn publish_progress_;

    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> abort_requested_{false};
};

} // namespace upc::glavo

#endif

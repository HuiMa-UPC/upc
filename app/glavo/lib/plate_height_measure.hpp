#ifndef UPC_PLATE_HEIGHT_MEASURE_HPP
#define UPC_PLATE_HEIGHT_MEASURE_HPP

/**
 * @file plate_height_measure.hpp
 * @brief 打印前两步测高：粗 LUT → AXIS_INC → 精测 30μm/200μm 门。失败不阻断打印。
 *        回零先下降 20mm 再 Home；开始关灯；结束不开舱灯。
 */

#include "galvo_motion_encode.hpp"
#include "height_lut_2d.hpp"
#include "height_points.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace upc::glavo {

struct PlateHeightMeasureResult {
    bool   ran = false;
    bool   ok = false;
    bool   flatness_warn = false;
    float  h_med_mm = 0.0f;
};

class PlateHeightMeasure {
public:
    using PublishSessionFn = std::function<void(bool active)>;
    using DoneFn = std::function<void(const PlateHeightMeasureResult&)>;

    PlateHeightMeasure() = default;
    ~PlateHeightMeasure();

    PlateHeightMeasure(const PlateHeightMeasure&) = delete;
    PlateHeightMeasure& operator=(const PlateHeightMeasure&) = delete;

    void configure(HeightMoveGalvoFn move_galvo,
                   HeightSessionLightFn set_session_light,
                   HeightSendZHomeFn send_z_home,
                   HeightSendZIncFn send_z_inc,
                   HeightWaitZFn wait_z,
                   HeightRequestSpotFn request_spot,
                   PublishSessionFn publish_session,
                   DoneFn on_done);

    /** 调用前须已确认 LUT 可读。@return false=已在跑或未配置 */
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
    Scan scan(const HeightLut2d& lut, const char* tag);

    HeightMoveGalvoFn move_galvo_;
    HeightSessionLightFn set_session_light_;
    HeightSendZHomeFn send_z_home_;
    HeightSendZIncFn send_z_inc_;
    HeightWaitZFn wait_z_;
    HeightRequestSpotFn request_spot_;
    PublishSessionFn publish_session_;
    DoneFn on_done_;

    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> abort_requested_{false};
};

} // namespace upc::glavo

#endif

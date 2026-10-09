#ifndef UPC_PLATE_HEIGHT_MEASURE_HPP
#define UPC_PLATE_HEIGHT_MEASURE_HPP

/**
 * @file plate_height_measure.hpp
 * @brief 打印前测高一次做完三件事：粗测 8 点光路、H-1 在位、再调高度。
 *        粗测必须 8 个点都看到光斑，少一个即光路失败。精测仍可跳过个别点。
 *        回零直接发 Z Home，不再先相对下降；开始关灯；结束不开舱灯。
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
    /** 粗测 H-1 不超过门限：没放基板。此时未做粗调 AXIS_INC。 */
    bool   plate_absent = false;
    /** 粗测 8 点里有点没看到光斑。不算没板，也不算查表失败。 */
    bool   optical_fail = false;
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

    /**
     * 调用前须已确认 LUT 可读。回零后的粗测始终用 H-1 判断有没有基板。
     * @return false=已在跑或未配置
     */
    bool start(const GalvoMotionCalibration& cal);

    void abort();
    [[nodiscard]] bool active() const noexcept { return active_.load(); }
    [[nodiscard]] bool cancel_requested() const noexcept {
        return abort_requested_.load();
    }

private:
    /** 粗测 require_all 时：没光斑是光路失败，有光斑但查表 NaN 是测高失败。 */
    enum class ScanStop { Ok, Aborted, Optical, Lookup };

    struct Scan {
        float h_med = 0.0f;
        float max_dev = 0.0f;
        int n = 0;
        ScanStop stop = ScanStop::Ok;
    };

    void run(GalvoMotionCalibration cal);
    Scan scan(const HeightLut2d& lut, const char* tag, bool require_all);

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

/**
 * @file height_calibration.cpp
 * @brief 调试页高度校准：网格采点后一次性写 LUT。
 */
#include "height_calibration.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <vector>

namespace upc::glavo {

HeightCalibration::~HeightCalibration()
{
    abort();
}

void HeightCalibration::configure(HeightMoveGalvoFn move_galvo,
                                  HeightSessionLightFn set_session_light,
                                  HeightSendZHomeFn send_z_home,
                                  HeightSendZIncFn send_z_inc,
                                  HeightWaitZFn wait_z,
                                  HeightRequestSpotFn request_spot,
                                  PublishProgressFn publish_progress)
{
    move_galvo_ = std::move(move_galvo);
    set_session_light_ = std::move(set_session_light);
    send_z_home_ = std::move(send_z_home);
    send_z_inc_ = std::move(send_z_inc);
    wait_z_ = std::move(wait_z);
    request_spot_ = std::move(request_spot);
    publish_progress_ = std::move(publish_progress);
}

bool HeightCalibration::start(const GalvoMotionCalibration& cal,
                              float start_mm, float end_mm, float step_mm,
                              std::string lut_path)
{
    (void)cal;
    if(!move_galvo_ || !set_session_light_ || !send_z_home_ || !send_z_inc_ ||
       !wait_z_ || !request_spot_ || !publish_progress_) {
        UPC_LOG_GLAVO_SERVICE(Error, "height calib start rejected: not configured");
        return false;
    }
    bool expected = false;
    if(!active_.compare_exchange_strong(expected, true)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "height calib already running");
        return false;
    }
    if(worker_.joinable()) {
        worker_.join();
    }
    abort_requested_.store(false);
    if(step_mm <= 0.0f) {
        step_mm = 1.0f;
    }
    if(end_mm < start_mm) {
        std::swap(start_mm, end_mm);
    }
    if(lut_path.empty()) {
        lut_path = kHeightLutPath;
    }
    worker_ = std::thread([this, start_mm, end_mm, step_mm, lut_path]() {
        run(start_mm, end_mm, step_mm, lut_path);
    });
    UPC_LOG_GLAVO_SERVICE(Info, "height calib started: %.1f..%.1f step %.1f -> %s",
                          static_cast<double>(start_mm),
                          static_cast<double>(end_mm),
                          static_cast<double>(step_mm), lut_path.c_str());
    return true;
}

void HeightCalibration::abort()
{
    abort_requested_.store(true);
    if(worker_.joinable()) {
        worker_.join();
    }
    active_.store(false);
}

void HeightCalibration::run(float start_mm, float end_mm, float step_mm,
                            std::string lut_path)
{
    std::vector<float> calib_heights;
    for(float h = start_mm; h <= end_mm + 1e-4f; h += step_mm) {
        calib_heights.push_back(h);
    }
    constexpr float kFineHeights[] = {
        -1.0f, -0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f
    };
    for(float fh : kFineHeights) {
        if(fh + 1e-4f < start_mm || fh > end_mm + 1e-4f) {
            continue;
        }
        bool dup = false;
        for(float x : calib_heights) {
            if(std::fabs(x - fh) < 0.05f) {
                dup = true;
                break;
            }
        }
        if(!dup) {
            calib_heights.push_back(fh);
        }
    }
    std::sort(calib_heights.begin(), calib_heights.end());
    const uint16_t total_count = static_cast<uint16_t>(calib_heights.size());
    publish_progress_(0, 0.0f, 0, total_count);

    uint8_t result_state = 2;
    do {
        set_session_light_(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(kHeightRedSettleMs));
        if(abort_requested_.load()) {
            result_state = 1;
            break;
        }

        send_z_home_();
        if(!wait_z_(kHeightZHomeMm, 0.1f, kHeightZHomeTimeoutMs)) {
            if(abort_requested_.load()) {
                result_state = 1;
                break;
            }
            UPC_LOG_GLAVO_SERVICE(Warn,
                "height calib: z home failed (expect MCU Z=%.2f)",
                static_cast<double>(kHeightZHomeMm));
            break;
        }
        if(abort_requested_.load()) {
            result_state = 1;
            break;
        }

        const float pre_dive = start_mm - 5.0f;
        height_send_z_inc(send_z_inc_, pre_dive);
        if(!wait_z_(height_mcu_z(pre_dive), 0.1f, kHeightZHomeTimeoutMs)) {
            if(abort_requested_.load()) {
                result_state = 1;
                break;
            }
            UPC_LOG_GLAVO_SERVICE(Warn,
                "height calib: pre-dive settle failed logical=%.2f mcu=%.2f",
                static_cast<double>(pre_dive),
                static_cast<double>(height_mcu_z(pre_dive)));
            break;
        }
        if(abort_requested_.load()) {
            result_state = 1;
            break;
        }

        struct CalibSample {
            float height;
            float galvo_x;
            float galvo_y;
            float laser_x;
            float laser_y;
        };
        std::vector<CalibSample> samples;
        samples.reserve(static_cast<std::size_t>(total_count) * kHeightMeasureCount);
        uint16_t collected = 0;
        bool aborted = false;
        float z_cur = pre_dive;

        for(float h : calib_heights) {
            if(abort_requested_.load()) {
                aborted = true;
                break;
            }
            height_send_z_inc(send_z_inc_, h - z_cur);
            z_cur = h;
            if(!wait_z_(height_mcu_z(h), 0.05f, kHeightZHomeTimeoutMs)) {
                if(abort_requested_.load()) {
                    aborted = true;
                    break;
                }
                UPC_LOG_GLAVO_SERVICE(Warn,
                    "calib: z settle failed at logical=%.2f mcu=%.2f, skip",
                    static_cast<double>(h),
                    static_cast<double>(height_mcu_z(h)));
                continue;
            }
            for(const auto& pt : kHeightMeasurePoints) {
                float ix = 0.0f, iy = 0.0f;
                const HeightSpotRc rc = sample_galvo_spot(
                    move_galvo_, request_spot_, abort_requested_, pt.x, pt.y, ix, iy);
                if(rc == HeightSpotRc::Aborted) {
                    aborted = true;
                    break;
                }
                if(rc != HeightSpotRc::Ok) {
                    UPC_LOG_GLAVO_SERVICE(Warn,
                        "calib: spot skip at h=%.2f (%.1f,%.1f) rc=%d",
                        static_cast<double>(h), pt.x, pt.y, static_cast<int>(rc));
                    continue;
                }
                samples.push_back({h, pt.x, pt.y, ix, iy});
                UPC_LOG_GLAVO_SERVICE(Info,
                    "calib point: h=%.2f gx=%.1f gy=%.1f x=%.3f y=%.3f",
                    static_cast<double>(h), pt.x, pt.y,
                    static_cast<double>(ix), static_cast<double>(iy));
            }
            if(aborted) {
                break;
            }
            ++collected;
            publish_progress_(1, h, collected, total_count);
        }
        if(aborted) {
            result_state = 1;
            break;
        }
        if(samples.empty()) {
            UPC_LOG_GLAVO_SERVICE(Error, "calib: no samples collected, abort");
            break;
        }
        std::sort(samples.begin(), samples.end(),
                  [](const CalibSample& a, const CalibSample& b) {
                      if(a.height != b.height) return a.height < b.height;
                      if(a.galvo_x != b.galvo_x) return a.galvo_x < b.galvo_x;
                      return a.galvo_y < b.galvo_y;
                  });
        {
            const std::string tmp_path = lut_path + ".tmp";
            std::ofstream f(tmp_path);
            if(!f.is_open()) {
                UPC_LOG_GLAVO_SERVICE(Error, "calib: lut open failed: %s",
                                      tmp_path.c_str());
                break;
            }
            f << "# 8-point galvo calibration: height galvo_x galvo_y img_x img_y\n";
            f << "# auto generated by height calibration tool\n";
            f << std::fixed;
            f.precision(3);
            for(const auto& s : samples) {
                f << s.height << "\t" << s.galvo_x << "\t" << s.galvo_y
                  << "\t" << s.laser_x << "\t" << s.laser_y << "\n";
            }
            if(!f) {
                UPC_LOG_GLAVO_SERVICE(Error, "calib: lut write failed: %s",
                                      tmp_path.c_str());
                f.close();
                std::remove(tmp_path.c_str());
                break;
            }
            f.close();
            if(std::rename(tmp_path.c_str(), lut_path.c_str()) != 0) {
                UPC_LOG_GLAVO_SERVICE(Error, "calib: lut rename failed: %s -> %s",
                                      tmp_path.c_str(), lut_path.c_str());
                std::remove(tmp_path.c_str());
                break;
            }
        }
        UPC_LOG_GLAVO_SERVICE(Info, "calib: lut saved %s (%zu points)",
                              lut_path.c_str(), samples.size());
        result_state = 0;
    } while(false);

    set_session_light_(false);

    if(!abort_requested_.load()) {
        send_z_home_();
        if(!wait_z_(kHeightZHomeMm, 0.1f, kHeightZHomeTimeoutMs)) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "height calib: return to home plane MCU Z=%.2f failed",
                static_cast<double>(kHeightZHomeMm));
        }
    }

    if(result_state == 0) {
        publish_progress_(2, 0.0f, 0, total_count);
        UPC_LOG_GLAVO_SERVICE(Info, "height calibration done");
    } else if(result_state == 1) {
        publish_progress_(3, 0.0f, 0, total_count);
        UPC_LOG_GLAVO_SERVICE(Warn, "height calibration aborted");
    } else {
        publish_progress_(3, 0.0f, 0, total_count);
        UPC_LOG_GLAVO_SERVICE(Error, "height calibration failed");
    }
    active_.store(false);
}

} // namespace upc::glavo

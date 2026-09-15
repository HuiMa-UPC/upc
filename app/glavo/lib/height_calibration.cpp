/**
 * @file height_calibration.cpp
 * @brief 激光高度校准状态机：关舱灯、开红光、AXIS_INC 网格采集、写 LUT。
 */
#include "height_calibration.hpp"

#include "galvo_motion_encode.hpp"
#include "height_points.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <vector>

namespace upc::glavo {

HeightCalibration::~HeightCalibration()
{
    abort();
}

void HeightCalibration::configure(SendMotionFn send_motion,
                                  PrepareIlluminationFn prepare_illumination,
                                  SendZHomeFn send_z_home,
                                  SendZIncFn send_z_inc,
                                  WaitZFn wait_z,
                                  RequestSpotFn request_spot,
                                  PublishProgressFn publish_progress,
                                  TurnRedOffFn turn_red_off)
{
    send_motion_ = std::move(send_motion);
    prepare_illumination_ = std::move(prepare_illumination);
    send_z_home_ = std::move(send_z_home);
    send_z_inc_ = std::move(send_z_inc);
    wait_z_ = std::move(wait_z);
    request_spot_ = std::move(request_spot);
    publish_progress_ = std::move(publish_progress);
    turn_red_off_ = std::move(turn_red_off);
}

bool HeightCalibration::start(const GalvoMotionCalibration& cal,
                              float start_mm, float end_mm, float step_mm,
                              std::string lut_path)
{
    if(!send_motion_ || !prepare_illumination_ || !send_z_home_ || !send_z_inc_ ||
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
    GalvoMotionCalibration snapshot = cal;
    worker_ = std::thread([this, snapshot, start_mm, end_mm, step_mm, lut_path]() {
        run(snapshot, start_mm, end_mm, step_mm, lut_path);
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

void HeightCalibration::run(GalvoMotionCalibration cal, float start_mm,
                            float end_mm, float step_mm, std::string lut_path)
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        if(abort_requested_.load()) {
            result_state = 1;
            break;
        }
        if(!prepare_illumination_()) {
            UPC_LOG_GLAVO_SERVICE(Warn, "height calib: illumination failed");
            break;
        }
        if(abort_requested_.load()) {
            result_state = 1;
            break;
        }

        send_z_home_();
        if(!wait_z_(0.0f, 0.1f, kHeightZHomeTimeoutMs)) {
            break;
        }
        if(abort_requested_.load()) {
            result_state = 1;
            break;
        }

        const float pre_dive = start_mm - 5.0f;
        send_z_inc_(pre_dive);
        if(!wait_z_(pre_dive, 0.1f, kHeightZHomeTimeoutMs)) {
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
            send_z_inc_(h - z_cur);
            z_cur = h;
            if(!wait_z_(h, 0.05f, kHeightZHomeTimeoutMs)) {
                UPC_LOG_GLAVO_SERVICE(Warn,
                    "calib: z settle failed at %.2f, skip", static_cast<double>(h));
                continue;
            }
            for(const auto& pt : kHeightMeasurePoints) {
                if(abort_requested_.load()) {
                    aborted = true;
                    break;
                }
                const auto packet = galvo_encode_motion_subpacket(cal, pt.x, pt.y);
                if(!send_motion_(packet.data())) {
                    UPC_LOG_GLAVO_SERVICE(Warn,
                        "calib: galvo move failed at h=%.2f (%.1f,%.1f), skip",
                        static_cast<double>(h), pt.x, pt.y);
                    continue;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kHeightGalvoSettleMs));
                float ix = 0.0f, iy = 0.0f;
                if(!request_spot_(ix, iy, kHeightSpotTimeoutMs)) {
                    UPC_LOG_GLAVO_SERVICE(Warn,
                        "calib: spot measure failed at h=%.2f (%.1f,%.1f), skip",
                        static_cast<double>(h), pt.x, pt.y);
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
            std::ofstream f(lut_path);
            if(!f.is_open()) {
                UPC_LOG_GLAVO_SERVICE(Error, "calib: lut open failed: %s",
                                      lut_path.c_str());
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
            if(!f.good()) {
                UPC_LOG_GLAVO_SERVICE(Error, "calib: lut write failed: %s",
                                      lut_path.c_str());
                break;
            }
        }
        UPC_LOG_GLAVO_SERVICE(Info, "calib: lut saved %s (%zu points)",
                              lut_path.c_str(), samples.size());
        result_state = 0;
    } while(false);

    if(turn_red_off_) {
        turn_red_off_();
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

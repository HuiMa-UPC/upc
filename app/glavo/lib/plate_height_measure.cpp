/**
 * @file plate_height_measure.cpp
 * @brief 打印前 8 点两步测高并 AXIS_INC 调 Z。
 */
#include "plate_height_measure.hpp"

#include "height_points.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>

namespace upc::glavo {

PlateHeightMeasure::~PlateHeightMeasure()
{
    abort();
}

void PlateHeightMeasure::configure(SendMotionFn send_motion,
                                   PrepareIlluminationFn prepare_illumination,
                                   SendZHomeFn send_z_home,
                                   SendZIncFn send_z_inc,
                                   WaitZFn wait_z,
                                   RequestSpotFn request_spot,
                                   PublishSessionFn publish_session,
                                   DoneFn on_done)
{
    send_motion_ = std::move(send_motion);
    prepare_illumination_ = std::move(prepare_illumination);
    send_z_home_ = std::move(send_z_home);
    send_z_inc_ = std::move(send_z_inc);
    wait_z_ = std::move(wait_z);
    request_spot_ = std::move(request_spot);
    publish_session_ = std::move(publish_session);
    on_done_ = std::move(on_done);
}

bool PlateHeightMeasure::start(const GalvoMotionCalibration& cal)
{
    if(!send_motion_ || !prepare_illumination_ || !send_z_home_ || !send_z_inc_ ||
       !wait_z_ || !request_spot_ || !on_done_) {
        UPC_LOG_GLAVO_SERVICE(Error, "plate height start rejected: not configured");
        return false;
    }
    bool expected = false;
    if(!active_.compare_exchange_strong(expected, true)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "plate height already active");
        return false;
    }
    if(worker_.joinable()) {
        worker_.join();
    }
    abort_requested_.store(false);
    const uint32_t generation = generation_.fetch_add(1) + 1;
    (void)generation;
    GalvoMotionCalibration snapshot = cal;
    worker_ = std::thread([this, snapshot]() { run(snapshot); });
    return true;
}

void PlateHeightMeasure::abort()
{
    generation_.fetch_add(1);
    abort_requested_.store(true);
    if(worker_.joinable()) {
        worker_.join();
    }
    active_.store(false);
}

PlateHeightMeasure::Scan PlateHeightMeasure::scan(
    const HeightLut2d& lut, const GalvoMotionCalibration& cal, const char* tag)
{
    Scan out;
    out.h_med = std::nanf("");
    out.max_dev = std::nanf("");
    out.n = 0;

    std::vector<float> hs;
    hs.reserve(static_cast<std::size_t>(kHeightMeasureCount));
    for(const auto& pt : kHeightMeasurePoints) {
        if(abort_requested_.load()) {
            break;
        }
        const auto packet = galvo_encode_motion_subpacket(cal, pt.x, pt.y);
        if(!send_motion_(packet.data())) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "measure[%s]: galvo move failed (%.1f,%.1f), skip", tag, pt.x, pt.y);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kHeightGalvoSettleMs));
        float ix = 0.0f, iy = 0.0f;
        if(!request_spot_(ix, iy, kHeightSpotTimeoutMs)) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "measure[%s]: spot failed (%.1f,%.1f), skip", tag, pt.x, pt.y);
            continue;
        }
        const float h = lut.lookup(pt.x, pt.y, ix, iy);
        if(std::isnan(h)) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "measure[%s]: lookup nan (%.1f,%.1f) ix=%.3f iy=%.3f, skip",
                tag, pt.x, pt.y, static_cast<double>(ix), static_cast<double>(iy));
            continue;
        }
        hs.push_back(h);
        UPC_LOG_GLAVO_SERVICE(Info,
            "measure[%s] (%.1f,%.1f) ix=%.3f iy=%.3f h=%.3f",
            tag, pt.x, pt.y, static_cast<double>(ix), static_cast<double>(iy),
            static_cast<double>(h));
    }

    const int n = static_cast<int>(hs.size());
    out.n = n;
    if(n == 0) {
        UPC_LOG_GLAVO_SERVICE(Error, "measure[%s]: no valid points", tag);
        return out;
    }
    std::sort(hs.begin(), hs.end());
    out.h_med = (n % 2 != 0)
        ? hs[static_cast<std::size_t>(n / 2)]
        : (hs[static_cast<std::size_t>(n / 2 - 1)] +
           hs[static_cast<std::size_t>(n / 2)]) * 0.5f;
    float max_dev = 0.0f;
    for(float hi : hs) {
        const float d = std::fabs(hi - out.h_med);
        if(d > max_dev) {
            max_dev = d;
        }
    }
    out.max_dev = max_dev;
    UPC_LOG_GLAVO_SERVICE(Info,
        "measure[%s]: h_med=%.3f mm (n=%d/%d) max_dev=%.3f mm",
        tag, static_cast<double>(out.h_med), n, kHeightMeasureCount,
        static_cast<double>(out.max_dev));
    return out;
}

void PlateHeightMeasure::run(GalvoMotionCalibration cal)
{
    if(publish_session_) {
        publish_session_(true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    PlateHeightMeasureResult result;
    result.ran = true;

    do {
        if(abort_requested_.load()) {
            break;
        }
        if(!prepare_illumination_()) {
            UPC_LOG_GLAVO_SERVICE(Warn, "plate height: illumination failed");
            break;
        }
        if(abort_requested_.load()) {
            break;
        }

        send_z_home_();
        if(!wait_z_(0.0f, 0.1f, kHeightZHomeTimeoutMs)) {
            UPC_LOG_GLAVO_SERVICE(Warn, "measure_plate_height: z home failed");
            break;
        }
        if(abort_requested_.load()) {
            break;
        }

        HeightLut2d lut_coarse;
        if(!lut_coarse.load(kHeightLutPath) || !lut_coarse.valid()) {
            UPC_LOG_GLAVO_SERVICE(Error, "measure_plate_height: coarse lut load failed");
            break;
        }
        HeightLut2d lut_fine;
        if(!lut_fine.load(kHeightLutPath, kFineLutMinMm, kFineLutMaxMm)
           || !lut_fine.valid()) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "measure_plate_height: fine lut [%.1f,%.1f] unavailable, reuse coarse",
                static_cast<double>(kFineLutMinMm),
                static_cast<double>(kFineLutMaxMm));
        }

        const Scan coarse = scan(lut_coarse, cal, "coarse");
        if(std::isnan(coarse.h_med)) {
            break;
        }
        const float delta1 = -coarse.h_med;
        send_z_inc_(delta1);
        float z_now = delta1;
        (void)wait_z_(z_now, 0.15f, kHeightZHomeTimeoutMs);

        const HeightLut2d& fine = lut_fine.valid() ? lut_fine : lut_coarse;
        float last_h = std::nanf("");
        float pending = std::nanf("");
        bool agree = false;
        auto apply_inc = [&](float h_med) {
            const float d = -h_med;
            send_z_inc_(d);
            z_now += d;
            (void)wait_z_(z_now, 0.15f, kHeightZHomeTimeoutMs);
            return d;
        };

        for(int round = 1; round <= kHeightAgreeMaxRounds; ++round) {
            if(abort_requested_.load()) {
                break;
            }
            const Scan s = scan(fine, cal, "fine");
            if(s.n > 0) {
                last_h = s.h_med;
                pending = s.h_med;
            }
            if(s.n == 0) {
                UPC_LOG_GLAVO_SERVICE(Info,
                    "measure[fine]: round=%d/%d n=0/%d rescan (no median)",
                    round, kHeightAgreeMaxRounds, kHeightMeasureCount);
                continue;
            }
            const bool close = (std::fabs(pending) < kHeightCloseMm);
            const bool aligned = (s.n == kHeightMeasureCount && !std::isnan(s.max_dev)
                                  && s.max_dev <= kHeightAgreeMm);
            if(!close) {
                const float d = apply_inc(pending);
                UPC_LOG_GLAVO_SERVICE(Info,
                    "measure[fine]: round=%d/%d |h|=%.3f mm max_dev=%.3f mm nudge d=%.3f",
                    round, kHeightAgreeMaxRounds,
                    static_cast<double>(pending),
                    static_cast<double>(s.max_dev),
                    static_cast<double>(d));
                pending = std::nanf("");
                continue;
            }
            if(aligned) {
                const float d = apply_inc(pending);
                UPC_LOG_GLAVO_SERVICE(Info,
                    "measure[fine]: round=%d/%d |h|=%.3f mm max_dev=%.3f mm pass d=%.3f",
                    round, kHeightAgreeMaxRounds,
                    static_cast<double>(pending),
                    static_cast<double>(s.max_dev),
                    static_cast<double>(d));
                pending = std::nanf("");
                agree = true;
                break;
            }
            UPC_LOG_GLAVO_SERVICE(Info,
                "measure[fine]: round=%d/%d |h|=%.3f mm max_dev=%.3f mm n=%d/%d rescan",
                round, kHeightAgreeMaxRounds,
                static_cast<double>(pending),
                static_cast<double>(s.max_dev), s.n, kHeightMeasureCount);
        }

        if(abort_requested_.load()) {
            break;
        }
        if(!agree) {
            UPC_LOG_GLAVO_SERVICE(Warn, "五次未通过，基板平整度需要检查");
            result.flatness_warn = true;
            if(!std::isnan(pending)) {
                apply_inc(pending);
                pending = std::nanf("");
            }
        }
        if(std::isnan(last_h)) {
            break;
        }
        result.ok = true;
        result.h_med_mm = last_h;
        UPC_LOG_GLAVO_SERVICE(Info, "measure_plate_height done h_med=%.3f mm",
                              static_cast<double>(last_h));
    } while(false);

    if(publish_session_) {
        publish_session_(false);
    }
    active_.store(false);
    if(abort_requested_.load()) {
        UPC_LOG_GLAVO_SERVICE(Info, "plate height round aborted");
        return;
    }
    on_done_(result);
}

} // namespace upc::glavo

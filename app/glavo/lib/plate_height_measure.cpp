/**
 * @file plate_height_measure.cpp
 * @brief 打印前 8 点两步测高并 AXIS_INC 调 Z。
 */
#include "plate_height_measure.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace upc::glavo {

PlateHeightMeasure::~PlateHeightMeasure()
{
    abort();
}

void PlateHeightMeasure::configure(HeightMoveGalvoFn move_galvo,
                                   HeightSessionLightFn set_session_light,
                                   HeightSendZHomeFn send_z_home,
                                   HeightSendZIncFn send_z_inc,
                                   HeightWaitZFn wait_z,
                                   HeightRequestSpotFn request_spot,
                                   PublishSessionFn publish_session,
                                   DoneFn on_done)
{
    move_galvo_ = std::move(move_galvo);
    set_session_light_ = std::move(set_session_light);
    send_z_home_ = std::move(send_z_home);
    send_z_inc_ = std::move(send_z_inc);
    wait_z_ = std::move(wait_z);
    request_spot_ = std::move(request_spot);
    publish_session_ = std::move(publish_session);
    on_done_ = std::move(on_done);
}

bool PlateHeightMeasure::start(const GalvoMotionCalibration& cal)
{
    if(!move_galvo_ || !set_session_light_ || !send_z_home_ || !send_z_inc_ ||
       !wait_z_ || !request_spot_ || !publish_session_ || !on_done_) {
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
    GalvoMotionCalibration snapshot = cal;
    worker_ = std::thread([this, snapshot]() { run(snapshot); });
    return true;
}

void PlateHeightMeasure::abort()
{
    abort_requested_.store(true);
    if(worker_.joinable()) {
        worker_.join();
    }
    active_.store(false);
}

PlateHeightMeasure::Scan PlateHeightMeasure::scan(
    const HeightLut2d& lut, const char* tag)
{
    Scan out;
    out.h_med = std::nanf("");
    out.max_dev = std::nanf("");
    out.n = 0;

    std::vector<float> hs;
    hs.reserve(static_cast<std::size_t>(kHeightMeasureCount));
    for(const auto& pt : kHeightMeasurePoints) {
        float ix = 0.0f;
        float iy = 0.0f;
        const HeightSpotRc rc = sample_galvo_spot(
            move_galvo_, request_spot_, abort_requested_, pt.x, pt.y, ix, iy);
        if(rc == HeightSpotRc::Aborted) {
            break;
        }
        if(rc != HeightSpotRc::Ok) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "measure[%s]: spot skip (%.1f,%.1f) rc=%d",
                tag, pt.x, pt.y, static_cast<int>(rc));
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
    publish_session_(true);
    set_session_light_(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(kHeightRedSettleMs));

    PlateHeightMeasureResult result;
    result.ran = true;

    do {
        UPC_LOG_GLAVO_SERVICE(Info,
            "plate height galvo=abs_pos laser_on=1 scale=%.4f range=%.3f speed=%.3f",
            static_cast<double>(cal.scale_gain),
            static_cast<double>(cal.range_gain),
            static_cast<double>(cal.speed_gain));
        if(abort_requested_.load()) {
            break;
        }

        send_z_home_();
        if(!wait_z_(kHeightZHomeMm, 0.1f, kHeightZHomeTimeoutMs)) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "measure_plate_height: z home failed (expect MCU Z=%.2f)",
                static_cast<double>(kHeightZHomeMm));
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

        const Scan coarse = scan(lut_coarse, "coarse");
        if(abort_requested_.load() || std::isnan(coarse.h_med)) {
            break;
        }
        const float delta1 = -coarse.h_med;
        height_send_z_inc(send_z_inc_, delta1);
        float z_now = height_mcu_z(delta1);
        if(!wait_z_(z_now, 0.15f, kHeightZHomeTimeoutMs)) {
            if(!abort_requested_.load()) {
                UPC_LOG_GLAVO_SERVICE(Warn,
                    "measure_plate_height: coarse z settle failed target=%.2f",
                    static_cast<double>(z_now));
            }
            break;
        }
        if(abort_requested_.load()) {
            break;
        }

        const HeightLut2d& fine = lut_fine.valid() ? lut_fine : lut_coarse;
        float last_h = std::nanf("");
        float pending = std::nanf("");
        bool agree = false;
        bool z_failed = false;
        auto apply_inc = [&](float h_med) -> bool {
            const float d = -h_med;
            height_send_z_inc(send_z_inc_, d);
            z_now += d;
            if(!wait_z_(z_now, 0.15f, kHeightZHomeTimeoutMs)) {
                if(!abort_requested_.load()) {
                    UPC_LOG_GLAVO_SERVICE(Warn,
                        "measure_plate_height: fine z settle failed target=%.2f d=%.3f",
                        static_cast<double>(z_now), static_cast<double>(d));
                }
                z_failed = true;
                return false;
            }
            return true;
        };

        for(int round = 1; round <= kHeightAgreeMaxRounds; ++round) {
            if(abort_requested_.load()) {
                break;
            }
            const Scan s = scan(fine, "fine");
            if(abort_requested_.load()) {
                break;
            }
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
                const float d = -pending;
                if(!apply_inc(pending)) {
                    break;
                }
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
                const float d = -pending;
                if(!apply_inc(pending)) {
                    break;
                }
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

        if(abort_requested_.load() || z_failed) {
            break;
        }
        if(!agree) {
            UPC_LOG_GLAVO_SERVICE(Warn, "五次未通过，基板平整度需要检查");
            result.flatness_warn = true;
            if(!std::isnan(pending)) {
                if(!apply_inc(pending)) {
                    break;
                }
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

    set_session_light_(false);
    publish_session_(false);
    active_.store(false);
    if(abort_requested_.load()) {
        UPC_LOG_GLAVO_SERVICE(Info, "plate height round aborted");
        return;
    }
    on_done_(result);
}

} // namespace upc::glavo

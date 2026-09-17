#ifndef UPC_HEIGHT_POINTS_HPP
#define UPC_HEIGHT_POINTS_HPP

/**
 * @file height_points.hpp
 * @brief 基板测高 / 高度校准共用点表、门限与单点采样。
 *
 * 与光路检测 kGalvoTestPoints（中心+四角）分开：测高 8 点 y≥0。
 * 改点表必须重新校准。
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

namespace upc::glavo {

struct HeightGalvoPoint {
    float x;
    float y;
};

constexpr std::array<HeightGalvoPoint, 8> kHeightMeasurePoints{{
    { 20.0f,  0.0f}, { 30.0f,  0.0f},
    { 30.0f, 30.0f}, { 20.0f, 20.0f},
    {-20.0f,  0.0f}, {-30.0f,  0.0f},
    {-30.0f, 30.0f}, {-20.0f, 20.0f},
}};

constexpr int kHeightMeasureCount =
    static_cast<int>(kHeightMeasurePoints.size());

constexpr int kHeightGalvoSettleMs = 300;
constexpr int kHeightSpotTimeoutMs = 5000;
constexpr int kHeightRedSettleMs = 500;
constexpr int kHeightZHomeTimeoutMs = 30000;

/** MCU Z Home 停点(mm)。逻辑 0 = 此平面；wait_z 用 MCU 上报值须加此偏置。 */
constexpr float kHeightZHomeMm = 1.0f;

inline float height_mcu_z(float logical_mm) noexcept
{
    return logical_mm + kHeightZHomeMm;
}

constexpr float kFineLutMinMm = -1.0f;
constexpr float kFineLutMaxMm = 1.0f;
constexpr float kHeightCloseMm = 0.030f;
constexpr float kHeightAgreeMm = 0.200f;
constexpr int kHeightAgreeMaxRounds = 5;

constexpr const char* kHeightLutPath = "/etc/visual/height_lut.txt";

using HeightMoveGalvoFn = std::function<bool(float x_mm, float y_mm)>;
/** true=关舱灯+Red_On；false=只开舱灯（红光保持）。 */
using HeightSessionLightFn = std::function<void(bool on)>;
using HeightSendZHomeFn = std::function<void()>;
using HeightSendZIncFn = std::function<void(float delta_mm)>;
using HeightWaitZFn =
    std::function<bool(float target_mm, float tol_mm, int timeout_ms)>;
using HeightRequestSpotFn =
    std::function<bool(float& x, float& y, int timeout_ms)>;

enum class HeightSpotRc { Ok, Aborted, MoveFailed, SpotFailed };

/** 走到 (gx,gy)、settle、取质心。 */
inline HeightSpotRc sample_galvo_spot(const HeightMoveGalvoFn& move_galvo,
                                      const HeightRequestSpotFn& request_spot,
                                      const std::atomic<bool>& abort,
                                      float gx, float gy,
                                      float& ix, float& iy)
{
    if(abort.load()) {
        return HeightSpotRc::Aborted;
    }
    if(!move_galvo(gx, gy)) {
        return HeightSpotRc::MoveFailed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kHeightGalvoSettleMs));
    if(abort.load()) {
        return HeightSpotRc::Aborted;
    }
    ix = 0.0f;
    iy = 0.0f;
    if(!request_spot(ix, iy, kHeightSpotTimeoutMs)) {
        return HeightSpotRc::SpotFailed;
    }
    return HeightSpotRc::Ok;
}

inline void height_send_z_inc(const HeightSendZIncFn& send_z_inc, float delta_mm)
{
    if(std::fabs(delta_mm) > 1e-4f) {
        send_z_inc(delta_mm);
    }
}

} // namespace upc::glavo

#endif

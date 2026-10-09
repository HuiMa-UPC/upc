#ifndef UPC_HEIGHT_POINTS_HPP
#define UPC_HEIGHT_POINTS_HPP

/**
 * @file height_points.hpp
 * @brief 基板测高 / 高度校准共用点表、门限与单点采样。
 *
 * 与调试页点位测试 kGalvoTestPoints（中心+四角）分开：测高 8 点 y≥0。
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
/**
 * 单点等 Visual 光斑回包。
 * Visual 先等新帧最多 8s（visual_service.cpp kLaserSpotNoFrameWaitMs），
 * 再测 1.5s（kLaserSpotDetectMs）。这里加上 IPC 余量，避免还在等帧时 Glavo 先超时。
 */
constexpr int kHeightSpotTimeoutMs = 8000 + 1500 + 700;
constexpr int kHeightRedSettleMs = 500;
constexpr int kHeightZHomeTimeoutMs = 30000;

/** MCU Z Home 停点(mm)。逻辑 0 = 此平面；wait_z 用 MCU 上报值须加此偏置。 */
constexpr float kHeightZHomeMm = 1.0f;

/** 回零后粗测：逻辑高度 H 大于此值视为有基板（mm）。H 已相对 Z=1 平面，不再减 1。 */
constexpr float kPlatePresentMinAboveHomeMm = 3.0f;

/** 高度校准回零先相对下降（负向 AXIS_INC），再 Home。打印前测高直接 Home。Z 无绝对编码器。 */
constexpr float kHeightHomeRetractMm = 20.0f;
/** 下降行程等待：编码器在动则提前到点，否则超时后仍发 Home。 */
constexpr int kHeightHomeRetractWaitMs = 8000;

inline float height_mcu_z(float logical_mm) noexcept
{
    return logical_mm + kHeightZHomeMm;
}

constexpr float kFineLutMinMm = -1.0f;
constexpr float kFineLutMaxMm = 1.0f;
constexpr float kHeightCloseMm = 0.030f;
constexpr float kHeightAgreeMm = 0.200f;
constexpr int kHeightAgreeMaxRounds = 5;

constexpr const char* kHeightLutPath = "/userdata/visual/height_lut.txt";

using HeightMoveGalvoFn = std::function<bool(float x_mm, float y_mm)>;
/** true=关舱灯+Red_On；false=开舱灯（红光保持）。测高回调的 false 为空操作。 */
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

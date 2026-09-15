#ifndef UPC_HEIGHT_POINTS_HPP
#define UPC_HEIGHT_POINTS_HPP

/**
 * @file height_points.hpp
 * @brief 基板测高 / 高度校准共用的振镜点表与门限。
 *
 * 与光路检测的 kGalvoTestPoints（中心+四角）分开：测高只用 y≥0 的 8 点，
 * 避开 y<0 强反光。改点表后必须重新校准，旧 LUT 不能混用。
 */

#include <array>

namespace upc::glavo {

struct HeightGalvoPoint {
    float x;
    float y;
};

constexpr std::array<HeightGalvoPoint, 8> kHeightMeasurePoints{{
    { 30.0f,  0.0f}, { 35.0f,  0.0f},
    {-30.0f,  0.0f}, {-35.0f,  0.0f},
    { 30.0f, 10.0f}, { 30.0f, 20.0f},
    {-30.0f, 10.0f}, {-30.0f, 20.0f},
}};

constexpr int kHeightMeasureCount =
    static_cast<int>(kHeightMeasurePoints.size());

constexpr int kHeightGalvoSettleMs = 300;
constexpr int kHeightSpotTimeoutMs = 5000;
constexpr int kHeightRedSettleMs = 500;
constexpr int kHeightZHomeTimeoutMs = 30000;

constexpr float kFineLutMinMm = -1.0f;
constexpr float kFineLutMaxMm = 1.0f;
constexpr float kHeightCloseMm = 0.030f;
constexpr float kHeightAgreeMm = 0.200f;
constexpr int kHeightAgreeMaxRounds = 5;

constexpr const char* kHeightLutPath = "/etc/visual/height_lut.txt";

} // namespace upc::glavo

#endif

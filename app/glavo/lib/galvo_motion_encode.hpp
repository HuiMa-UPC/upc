/**
 * @file galvo_motion_encode.hpp
 * @brief 振镜调试运动包编码：把 G01 X/Y/F/E 按 GcodeWorker 同规则编成 11 字节子包。
 *
 * 使用者：
 *   - 调试页「Galvo Point Test」一次性红光点位测试（glavo_service.cpp）
 *
 * 两者都需要「走校准表的单点红光定位」，编码规则必须与正式打印完全一致，
 * 否则调试看到的点位和实际出光位置会有偏差。规则来源见 parser 的 GcodeWorker：
 *   cal_apply_xy() → × scale_gain → / range_gain → × 32767 + 32768 → u16 LE
 *
 * 线程约定：本文件全为纯函数，CalData 与三个增益由调用方按值传入。
 * 调用方须在启动时从 ParserContext 复制一份快照，避免与 cal.csv 热加载并发读取。
 */
#ifndef UPC_GALVO_MOTION_ENCODE_HPP
#define UPC_GALVO_MOTION_ENCODE_HPP

#include "cal_parser.hpp"

#include <array>
#include <cstdint>

namespace upc::glavo {

/** MCU 运动子包长度（协议固定 11 字节）。 */
constexpr std::size_t kMotionSubpacketSize = 11;

/** 单个观察点的振镜坐标（mm，未经校准变换）。 */
struct GalvoTestPoint {
    float x;
    float y;
};

/**
 * 红光观察点序列：中心 → 右上 → 右下 → 左下 → 左上。
 *
 * 只给调试页点位测试用。打印前光路改走测高的 8 个点，不再用这五个坐标。
 * ±30mm 是相机视野内可稳定成像的范围，改动前须先用调试页那个按钮确认五个点都还在画面里。
 */
constexpr std::array<GalvoTestPoint, 5> kGalvoTestPoints{{
    {  0.0f,   0.0f },
    { 30.0f,  30.0f },
    { 30.0f, -30.0f },
    {-30.0f, -30.0f },
    {-30.0f,  30.0f },
}};

/** 校准与增益快照：启动一轮测试/检测时复制一份，全程只读。 */
struct GalvoMotionCalibration {
    parser::CalData cal{};
    float scale_gain = 1.0f;
    float range_gain = 1.0f;
    float speed_gain = 1.0f;
};

/** @brief 从 ParserContext 复制当前校准数据与三个增益。 */
GalvoMotionCalibration galvo_motion_calibration_snapshot();

/**
 * @brief 将一个 G01 X/Y F1000 E1 定位动作编码为 11 字节运动子包
 * @param cal   校准与增益快照（按值传入，避免热加载竞争）
 * @param x,y   目标振镜坐标（mm，编码内部过校准表）
 * @return 可直接交给 McuLink::send_debug_motion_subpacket() 的子包
 * @note LD=1：在 TCP Red=3 常亮下途中保持瞄准红光；LD=0 会被 MCU 当空跳掐灭。
 */
std::array<uint8_t, kMotionSubpacketSize> galvo_encode_motion_subpacket(
    const GalvoMotionCalibration& cal, float x, float y);

} // namespace upc::glavo

#endif

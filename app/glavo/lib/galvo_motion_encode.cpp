/**
 * @file galvo_motion_encode.cpp
 * @brief 振镜调试运动包编码实现（点位测试与光路检测共用）。
 */
#include "galvo_motion_encode.hpp"

#include "parser_config.hpp"
#include "parser_context.hpp"

#include <algorithm>

namespace upc::glavo {

namespace {

/** 将整数钳位到 MCU 运动子包的 uint16 小端字段。 */
void encode_u16_clamped(int value, uint8_t* lo, uint8_t* hi)
{
    value = std::clamp(value, 0, 65535);
    *lo = static_cast<uint8_t>(value & 0xff);
    *hi = static_cast<uint8_t>((value >> 8) & 0xff);
}

} // namespace

GalvoMotionCalibration galvo_motion_calibration_snapshot()
{
    const parser::ParserContext& ctx = parser::ParserContext::instance();
    GalvoMotionCalibration snapshot;
    snapshot.cal = ctx.cal_data();
    snapshot.scale_gain = ctx.scale_gain();
    snapshot.range_gain = ctx.range_gain();
    snapshot.speed_gain = ctx.speed_gain();
    return snapshot;
}

std::array<uint8_t, kMotionSubpacketSize> galvo_encode_motion_subpacket(
    const GalvoMotionCalibration& cal, float x, float y)
{
    std::array<uint8_t, kMotionSubpacketSize> pkt{};
    pkt[0] = 0x01; /* 0x01=运动子包，0x02=功率子包 */

    float out_x = 0.0f;
    float out_y = 0.0f;
    parser::cal_apply_xy(&cal.cal, x, y, &out_x, &out_y);

    /* range_gain 为 0 时（cal.csv 缺失或解析失败）退回解析器默认值，
     * 否则下面的除法会把坐标炸成 inf 再钳到边界。 */
    float range_gain = cal.range_gain;
    if(range_gain < 1e-6f) {
        range_gain = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    const auto encode_coordinate = [range_gain](float coordinate,
                                                 uint8_t* lo, uint8_t* hi) {
        const float normalized = coordinate / range_gain;
        const int offset = static_cast<int>(normalized * 32767.0f + 32768.0f);
        encode_u16_clamped(offset, lo, hi);
    };

    encode_coordinate(out_x * cal.scale_gain, &pkt[1], &pkt[2]);
    encode_coordinate(out_y * cal.scale_gain, &pkt[3], &pkt[4]);
    /* pkt[5..6] 保留 0；速度固定 F1000（与设计约定的测试指令一致） */
    encode_u16_clamped(static_cast<int>(1000.0f * cal.speed_gain), &pkt[7], &pkt[8]);
    encode_u16_clamped(0, &pkt[9], &pkt[10]); /* E0：仅移动红光点 */
    return pkt;
}

} // namespace upc::glavo

/**
 * @file galvo_coord.cpp
 * @brief 振镜坐标编码公共工具实现（提取自 ovf_worker.cpp，行为零变化）。
 */

#include "galvo_coord.hpp"
#include "parser_config.hpp"

namespace upc::parser {

void int_to_u16_bytes(int value, uint8_t* low_byte, uint8_t* high_byte)
{
    if(value > 65535) {
        value = 65535;
    }
    if(value < 0) {
        value = 0;
    }
    *low_byte  = static_cast<uint8_t>(value & 0xFF);
    *high_byte = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void coordinate_to_offset_bytes(float coordinate_value, float image_rang_gain,
                                 uint8_t* low_byte, uint8_t* high_byte)
{
    if(image_rang_gain < 1e-6f) {
        image_rang_gain = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    if(coordinate_value > image_rang_gain) {
        coordinate_value = image_rang_gain;
    }
    if(coordinate_value < -image_rang_gain) {
        coordinate_value = -image_rang_gain;
    }

    const float normalized = coordinate_value / image_rang_gain;
    const int offset_value = static_cast<int>(normalized * 32767.0f + 32768.0f);
    int_to_u16_bytes(offset_value, low_byte, high_byte);
}

} // namespace upc::parser

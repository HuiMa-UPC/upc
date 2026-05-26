#include "gcode_coord_map.hpp"
#include "gcode_parse.hpp"

#include <cmath>

static int u16_from_bytes(uint8_t lo, uint8_t hi)
{
    return static_cast<int>(lo) | (static_cast<int>(hi) << 8);
}

static void u16_to_bytes(int value, uint8_t *lo, uint8_t *hi)
{
    if(value > 65535) {
        value = 65535;
    }
    if(value < 0) {
        value = 0;
    }
    *lo = static_cast<uint8_t>(value & 0xFF);
    *hi = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static float offset_bytes_to_scaled(uint8_t lo, uint8_t hi, float image_rang_gain)
{
    if(image_rang_gain < 1e-6f) {
        image_rang_gain = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    const int offset_value = u16_from_bytes(lo, hi);
    const float normalized = (static_cast<float>(offset_value) - 32768.0f) / 32767.0f;
    return normalized * image_rang_gain;
}

static void scaled_to_offset_bytes(float scaled_coord, float image_rang_gain,
                                 uint8_t *lo, uint8_t *hi)
{
    if(image_rang_gain < 1e-6f) {
        image_rang_gain = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    float coordinate_value = scaled_coord;
    if(coordinate_value > image_rang_gain) {
        coordinate_value = image_rang_gain;
    }
    if(coordinate_value < -image_rang_gain) {
        coordinate_value = -image_rang_gain;
    }
    const float normalized = coordinate_value / image_rang_gain;
    const int offset_value = static_cast<int>(normalized * 32767.0f + 32768.0f);
    u16_to_bytes(offset_value, lo, hi);
}

static void motion_packet_nudge_scaled(rb_packet_t *pkt, float d_ox, float d_oy, float image_rang_gain)
{
    if(!pkt || pkt->type != RB_PKT_MOTION) {
        return;
    }
    if(std::fabs(d_ox) < 1e-9f && std::fabs(d_oy) < 1e-9f) {
        return;
    }

    float ox = offset_bytes_to_scaled(pkt->data[1], pkt->data[2], image_rang_gain);
    float oy = offset_bytes_to_scaled(pkt->data[3], pkt->data[4], image_rang_gain);
    ox += d_ox;
    oy += d_oy;

    uint8_t lo = 0;
    uint8_t hi = 0;
    scaled_to_offset_bytes(ox, image_rang_gain, &lo, &hi);
    pkt->data[1] = lo;
    pkt->data[2] = hi;
    scaled_to_offset_bytes(oy, image_rang_gain, &lo, &hi);
    pkt->data[3] = lo;
    pkt->data[4] = hi;
}

void gcode_motion_packet_nudge_gcode(rb_packet_t *pkt, float dx_gcode, float dy_gcode,
                                     float image_scale_gain, float image_rang_gain)
{
    if(dx_gcode == 0.f && dy_gcode == 0.f) {
        return;
    }
    if(image_scale_gain < 1e-6f) {
        image_scale_gain = 1.f;
    }
    motion_packet_nudge_scaled(pkt, dx_gcode * image_scale_gain, dy_gcode * image_scale_gain,
                              image_rang_gain);
}

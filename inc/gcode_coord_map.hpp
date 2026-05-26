#ifndef GCODE_COORD_MAP_H
#define GCODE_COORD_MAP_H

#include "ring_buffer.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 按 G-code 平面增量平移运动子包 X/Y（内部乘 image_scale_gain） */
void gcode_motion_packet_nudge_gcode(rb_packet_t *pkt, float dx_gcode, float dy_gcode,
                                     float image_scale_gain, float image_rang_gain);

#ifdef __cplusplus
}
#endif

#endif

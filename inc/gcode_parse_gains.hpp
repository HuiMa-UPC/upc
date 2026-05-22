#ifndef GCODE_PARSE_GAINS_H
#define GCODE_PARSE_GAINS_H

#ifdef __cplusplus
extern "C" {
#endif

/** 默认与 docs/Qt、下位机 spead_gain 一致 */
#define GCODE_DEFAULT_IMAGE_SCALE_GAIN  1.0f
#define GCODE_DEFAULT_IMAGE_RANG_GAIN   100.0f
#define GCODE_DEFAULT_SPEED_GAIN        167.12f

void gcode_parse_gains_set_defaults(void);

#ifdef __cplusplus
}
#endif

#endif

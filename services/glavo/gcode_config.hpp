#ifndef GCODE_CONFIG_HPP
#define GCODE_CONFIG_HPP

/* ---------- 解析参数默认值 ---------- */
#ifndef GCODE_DEFAULT_IMAGE_SCALE_GAIN
#define GCODE_DEFAULT_IMAGE_SCALE_GAIN  1.0f
#endif

#ifndef GCODE_DEFAULT_IMAGE_RANG_GAIN
#define GCODE_DEFAULT_IMAGE_RANG_GAIN   100.0f
#endif

#ifndef GCODE_DEFAULT_SPEED_GAIN
#define GCODE_DEFAULT_SPEED_GAIN        167.12f
#endif

/* ---------- 解析调度参数 ---------- */
#ifndef GCODE_PARSE_CHUNK_LINES
#define GCODE_PARSE_CHUNK_LINES         100
#endif

#ifndef GCODE_PARSE_SEND_SYNC_LINES
#define GCODE_PARSE_SEND_SYNC_LINES     50
#endif

#ifndef GCODE_PARSE_CHUNK_INTERVAL_MS
#define GCODE_PARSE_CHUNK_INTERVAL_MS   10
#endif

#endif // GCODE_CONFIG_HPP

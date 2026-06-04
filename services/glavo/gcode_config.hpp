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
//解析搬运行数阈值参数
#ifndef GCODE_PARSE_CHUNK_LINES
#define GCODE_PARSE_CHUNK_LINES         100
#endif

//上报阈值参数
#ifndef GCODE_PARSE_SEND_SYNC_LINES
#define GCODE_PARSE_SEND_SYNC_LINES     50
#endif

//解析间隔
#ifndef GCODE_PARSE_CHUNK_INTERVAL_MS
#define GCODE_PARSE_CHUNK_INTERVAL_MS   10
#endif


#if SERVICES_DEBUG
#define GCODE_MOTION_COORD_DBG(fmt, ...) printf("[GcodeCoord] " fmt "\n", ##__VA_ARGS__)
#else
#define GCODE_MOTION_COORD_DBG(fmt, ...) ((void)0)
#endif

#endif // GCODE_CONFIG_HPP

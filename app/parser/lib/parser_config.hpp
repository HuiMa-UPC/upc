#ifndef UPC_PARSER_CONFIG_HPP
#define UPC_PARSER_CONFIG_HPP

/** 默认校准 CSV 路径 */
#ifndef CAL_DEFAULT_FILE_PATH
#define CAL_DEFAULT_FILE_PATH "/home/CAL/cal.csv"
#endif

/** 默认层厚（μm） */
#ifndef GCODE_DEFAULT_LAYER_UM
#define GCODE_DEFAULT_LAYER_UM 30u
#endif

/* ---------- 解析参数默认值 ---------- */
#ifndef GCODE_DEFAULT_IMAGE_SCALE_GAIN
#define GCODE_DEFAULT_IMAGE_SCALE_GAIN 1.0f
#endif

#ifndef GCODE_DEFAULT_IMAGE_RANG_GAIN
#define GCODE_DEFAULT_IMAGE_RANG_GAIN 100.0f
#endif

#ifndef GCODE_DEFAULT_SPEED_GAIN
#define GCODE_DEFAULT_SPEED_GAIN 167.12f
#endif

/* ---------- G-code 解析调度参数 ---------- */
#ifndef GCODE_PARSE_CHUNK_LINES
#define GCODE_PARSE_CHUNK_LINES 100
#endif

#ifndef GCODE_PARSE_SEND_SYNC_LINES
#define GCODE_PARSE_SEND_SYNC_LINES 50
#endif

#ifndef GCODE_PARSE_CHUNK_INTERVAL_MS
#define GCODE_PARSE_CHUNK_INTERVAL_MS 10
#endif

#endif /* UPC_PARSER_CONFIG_HPP */

#ifndef UPC_PARSER_CONFIG_HPP
#define UPC_PARSER_CONFIG_HPP

/** 默认校准 CSV 路径（默认值须与 common/config/cal_config.hpp 的 UPC_CAL_* 一致） */
#ifndef UPC_CAL_DIR
#define UPC_CAL_DIR  "/home/CAL"
#endif

#ifndef UPC_CAL_PATH
#define UPC_CAL_PATH  UPC_CAL_DIR "/cal.csv"
#endif

#ifndef CAL_DEFAULT_FILE_PATH
#define CAL_DEFAULT_FILE_PATH  UPC_CAL_PATH
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
// //每块解析行数
// #ifndef GCODE_PARSE_CHUNK_LINES
// #define GCODE_PARSE_CHUNK_LINES 100
// #endif

// //进程同步间隔行数
// #ifndef GCODE_PARSE_SEND_SYNC_LINES
// #define GCODE_PARSE_SEND_SYNC_LINES 50
// #endif

// //块间 sleep
// #ifndef GCODE_PARSE_CHUNK_INTERVAL_MS
// #define GCODE_PARSE_CHUNK_INTERVAL_MS 10
// #endif

//每块解析行数
#ifndef GCODE_PARSE_CHUNK_LINES
#define GCODE_PARSE_CHUNK_LINES 200
#endif

//进程同步间隔行数
#ifndef GCODE_PARSE_SEND_SYNC_LINES
#define GCODE_PARSE_SEND_SYNC_LINES 100
#endif

//块间 sleep
#ifndef GCODE_PARSE_CHUNK_INTERVAL_MS
#define GCODE_PARSE_CHUNK_INTERVAL_MS 5
#endif

/** 解析后运动指令队列容量（约 12B/包，65536≈768KB） */
#define RB_MAX_PACKETS    65536
#define RB_PKT_DATA_LEN   11

#endif /* UPC_PARSER_CONFIG_HPP */

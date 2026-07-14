#ifndef RS_LOG_HPP
#define RS_LOG_HPP

/**
 * parser 独立构建用日志桩（UPC_PARSER_STANDALONE）。
 *
 * 不依赖 common/debug，所有 UPC_LOG_* 展开为空操作。
 * 嵌入主工程时由 upc::debug 提供完整 rs_log.hpp，本文件不参与编译。
 */

#define UPC_LOG_CAL_PARSER(lvl, fmt, ...)     ((void)0)
#define UPC_LOG_PARSER_CONTEXT(lvl, fmt, ...) ((void)0)
#define UPC_LOG_GCODE_WORKER(lvl, fmt, ...)   ((void)0)
#define UPC_LOG_OVF_WORKER(lvl, fmt, ...)     ((void)0)

#endif /* RS_LOG_HPP */

#ifndef UPC_XFR_PATHS_HPP
#define UPC_XFR_PATHS_HPP

/**
 * 局域网 Xfer 上传暂存与配对 token。
 *
 *   /userdata/transport/
 *     .xfr_tokens
 *     （分块上传 .part 与完成前的临时文件）
 *
 * 正式切片落盘仍走 UPC_LOCAL_GCODE_DIR（见 gcode_manifest.hpp）。
 */

#ifndef UPC_XFR_STORAGE_DIR
#define UPC_XFR_STORAGE_DIR  "/userdata/transport"
#endif

#ifndef UPC_XFR_TOKEN_FILE
#define UPC_XFR_TOKEN_FILE  UPC_XFR_STORAGE_DIR "/.xfr_tokens"
#endif

#endif /* UPC_XFR_PATHS_HPP */

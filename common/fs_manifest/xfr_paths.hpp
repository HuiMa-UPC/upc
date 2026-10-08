#ifndef UPC_XFR_PATHS_HPP
#define UPC_XFR_PATHS_HPP

/**
 * 局域网 Xfer 上传暂存与配对 token。
 *
 *   /userdata/transport/
 *     .xfr_tokens
 *     auto_print          （设置页：切片软件下发后是否自动起印，缺=关；首字节 `'1'` 为开）
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

/** 切片软件下发后是否自动 `print/start`。缺文件视为关；仅首字节 `'1'` 为开。 */
#ifndef UPC_XFR_AUTO_PRINT_FILE
#define UPC_XFR_AUTO_PRINT_FILE  UPC_XFR_STORAGE_DIR "/auto_print"
#endif

#include <cstdio>
#include <sys/stat.h>

namespace upc {
namespace xfr {

inline bool auto_print_after_transfer_enabled()
{
    FILE *f = std::fopen(UPC_XFR_AUTO_PRINT_FILE, "r");
    if(!f) {
        return false;
    }
    const int c = std::fgetc(f);
    std::fclose(f);
    return c == '1';
}

inline bool set_auto_print_after_transfer(bool on)
{
    (void)::mkdir(UPC_XFR_STORAGE_DIR, 0755);
    FILE *f = std::fopen(UPC_XFR_AUTO_PRINT_FILE, "w");
    if(!f) {
        return false;
    }
    const int n = std::fputc(on ? '1' : '0', f);
    std::fclose(f);
    return n != EOF;
}

} // namespace xfr
} // namespace upc

#endif /* UPC_XFR_PATHS_HPP */

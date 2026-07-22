#ifndef UPC_PRINT_HISTORY_MANIFEST_HPP
#define UPC_PRINT_HISTORY_MANIFEST_HPP

/**
 * 打印历史（延时摄影）目录 manifest：/home/video/manifest.json
 * - 路径：common/fs_manifest/
 * - Camera：Finish/Abort remux 成功后 upsert 一条
 * - UI：进入历史页时直接读取（不经列表 IPC）
 *
 * 媒体目录命名：{job_name}_{YYYYMMDD_HHMMSS}/（目录内 1.png,2.png,... 与 thumb.rthm）
 */

#include <cstdint>
#include <string>
#include <vector>

namespace upc {

#ifndef UPC_PRINT_VIDEO_DIR
#define UPC_PRINT_VIDEO_DIR  "/home/video"
#endif

#ifndef UPC_PRINT_VIDEO_MANIFEST
#define UPC_PRINT_VIDEO_MANIFEST  UPC_PRINT_VIDEO_DIR "/manifest.json"
#endif

/** 与 UI 设计稿 PRINT COMPLETE / PRINT TERMINATED 对应 */
enum class PrintHistoryStatus : uint8_t {
    Complete   = 0,
    Terminated = 1,
};

struct PrintHistoryEntry {
    std::string         job_name;      /**< 显示名（gcode 基名，无扩展名） */
    int64_t             start_unix = 0; /**< Session_Start 墙钟秒 */
    uint32_t            duration_sec = 0;
    float               material_g = 0.f;
    PrintHistoryStatus  status = PrintHistoryStatus::Complete;
    std::string         frames_dir;    /**< 相对目录名，如 benchy_20260526_221300 */
    std::string         thumb;         /**< 相对路径，如 benchy_20260526_221300/thumb.rthm */
};

/** 从 path 读取 manifest；文件不存在返回空列表（仍返回 true） */
bool print_history_manifest_load(const std::string& path,
                                 std::vector<PrintHistoryEntry>& out);

/** 写入 manifest（覆盖）；建议调用方用临时文件 + rename 保证原子性 */
bool print_history_manifest_save(const std::string& path,
                                 const std::vector<PrintHistoryEntry>& entries);

/**
 * 按 frames_dir upsert；不存在则追加。
 * 同一次打印不会改 job_name；主键是带时间戳的目录名。
 */
void print_history_manifest_upsert(std::vector<PrintHistoryEntry>& entries,
                                   const PrintHistoryEntry& item);

/** status ↔ JSON 字符串 */
const char* print_history_status_str(PrintHistoryStatus s);
bool print_history_status_parse(const std::string& s, PrintHistoryStatus& out);

/**
 * 从 G-code config.json 按 job 基名查找 material_g。
 * name 可为 "benchy" 或 "benchy.gcode"；找不到返回 0。
 */
float print_history_lookup_material_g(const std::string& gcode_config_path,
                                      const std::string& job_basename);

} // namespace upc

#endif /* UPC_PRINT_HISTORY_MANIFEST_HPP */

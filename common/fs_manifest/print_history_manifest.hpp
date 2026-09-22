#ifndef UPC_PRINT_HISTORY_MANIFEST_HPP
#define UPC_PRINT_HISTORY_MANIFEST_HPP

/**
 * 打印历史（延时摄影 + 可选打印录像）目录清单：/userdata/media/video/config.json
 * - 路径：common/fs_manifest/
 * - Media：Finish/Abort remux 成功后 upsert 一条
 * - UI：进入历史页时直接读取（不经列表 IPC）
 *
 * 媒体目录命名：{job_name}/（目录内 1.jpg,2.jpg,... 与 thumb.rthm）
 * 同一 job_name 重复打印会覆盖目录与媒体文件。
 */

#include "media_paths.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace upc {

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
    uint32_t            layer_count = 0; /**< G-code 层数（M3000 标记数）；0=未知，UI 显示 "--" */
    PrintHistoryStatus  status = PrintHistoryStatus::Complete;
    std::string         frames_dir;    /**< 相对目录名，如 benchy */
    std::string         thumb;         /**< 延时摄影缩略图，相对路径，如 benchy/thumb.rthm */
    std::string         print_thumb;   /**< 打印录像缩略图，相对路径，如 benchy/print_thumb.rthm */
    std::string         video;         /**< 延时摄影视频，相对路径，如 benchy/timelapse.mp4；序列播放模式下可空 */
    std::string         print_video;   /**< 相对路径，如 benchy/print_h264.mp4 */
    bool                video_pending = false; /**< 1=后台仍在生成延时摄影视频 */
    /** 1=本次会话开了连续 H264，且 print_h264.mp4 尚未落盘（Print History 用） */
    bool                print_video_pending = false;
    /** 1=历史页应可见；0=结果页未退出前隐藏。
     * UI 兜底：若为 0 但已有可删媒体（盘满导致 commit 失败），仍显示。 */
    bool                committed = true;
    uint32_t            clean_first_png = 0; /**< 清粉首帧序号（含）；0=无清粉追加 */
    uint32_t            clean_last_png = 0;  /**< 清粉末帧序号（含）；0=无清粉追加 */
};

/** 从 path 读取清单；文件不存在返回空列表（仍返回 true） */
bool print_history_manifest_load(const std::string& path,
                                 std::vector<PrintHistoryEntry>& out);

/** 写入清单（覆盖）；建议调用方用临时文件 + rename 保证原子性 */
bool print_history_manifest_save(const std::string& path,
                                 const std::vector<PrintHistoryEntry>& entries);

/**
 * 按 frames_dir upsert；不存在则追加。
 * 同一 job_name 重复打印会覆盖旧条目（主键为 frames_dir）。
 */
void print_history_manifest_upsert(std::vector<PrintHistoryEntry>& entries,
                                   const PrintHistoryEntry& item);

/** status ↔ JSON 字符串 */
const char* print_history_status_str(PrintHistoryStatus s);
bool print_history_status_parse(const std::string& s, PrintHistoryStatus& out);

/**
 * 从 G-code config.json 按 job 基名查找打印统计（耗材克重 + 层数）。
 * name 可为 "benchy" 或 "benchy.gcode"；找不到时两个出参均置 0。
 * 两项同源，合并成一次查询避免重复读盘解析。
 */
void print_history_lookup_gcode_stats(const std::string& gcode_config_path,
                                      const std::string& job_basename,
                                      float& material_g,
                                      uint32_t& layer_count);

} // namespace upc

#endif /* UPC_PRINT_HISTORY_MANIFEST_HPP */

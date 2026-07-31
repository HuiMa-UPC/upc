#ifndef TIMELAPSE_UTIL_HPP
#define TIMELAPSE_UTIL_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace upc::media {

/** 延时摄影 / 清粉共用：NV12→PNG、PNG→MP4、RTHM 缩略图、PNG 序号扫描与清理。 */
namespace timelapse_util {

std::string sanitize_filename(const char* name);
bool mkdir_p(const char* path);
int run_shell_cmd(const char* cmd, const char* log_tag);

std::string frame_png_path(const std::string& session_dir, uint32_t frame_index);

/** 扫描目录内最大 N.png；无则返回 0。 */
uint32_t scan_max_png_index(const std::string& session_dir);

bool encode_nv12_to_png(const uint8_t* nv12, size_t bytes,
                        uint32_t width, uint32_t height,
                        const std::string& session_dir, uint32_t frame_index);

bool encode_png_sequence_to_mp4(const std::string& session_dir,
                                uint32_t frame_count, uint32_t fps,
                                const char* out_path);

bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                uint32_t width, uint32_t height,
                                const char* out_path);

/** 删除 [first_index, last_index] 闭区间内的 PNG。 */
void remove_png_range(const std::string& session_dir,
                      uint32_t first_index, uint32_t last_index);

/** 仅当目录为空时删除目录本身（非空不删）。 */
bool remove_empty_dir(const std::string& session_dir);

/**
 * 更新已有历史条目的 thumb/video（保留 duration/start/status/material）。
 * frames_dir 不存在时返回 false。
 */
bool update_history_media_paths(const std::string& frames_dir,
                                const std::string& thumb_rel,
                                const std::string& video_rel);

} // namespace timelapse_util

} // namespace upc::media

#endif /* TIMELAPSE_UTIL_HPP */

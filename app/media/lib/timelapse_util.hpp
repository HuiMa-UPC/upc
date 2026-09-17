#ifndef TIMELAPSE_UTIL_HPP
#define TIMELAPSE_UTIL_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace upc::media {

/** 延时摄影 / 清粉共用：NV12→PNG、PNG→MP4、RTHM 缩略图（dma32+RGA）、PNG 序号扫描与清理。 */
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

/** 将 print.h264 裸流封装为 MP4（无音频）。 */
bool remux_h264_to_mp4(const char* h264_path, uint32_t fps, const char* out_path);

/**
 * 判断 MP4 是否含 moov 索引盒，即是否真的可播放。
 *
 * ffmpeg 默认把 moov 写在全部 mdat 之后，收尾阶段被强杀时文件只剩 ftyp+mdat：
 * 体积照样很大，播放器却打不开。只看"文件非空"会把这类残缺文件判成成功。
 *
 * 实现上顺着顶层 box 链走，只读 box 头再按 size 跳过内容，不做全文件扫描。
 *
 * @param out_conclusive 出参：box 链是否完整走到文件尾。false 表示中途遇到
 *        非法长度或读失败，本次结论不可信，调用方应退回保守处理（当作可用），
 *        以免把正常录像误丢。
 * @return 是否找到 moov。
 */
bool mp4_has_moov_box(const char* path, bool* out_conclusive);

/**
 * NV12→RTHM：dma-heap(dma32) + importbuffer_fd，与 UI 预览同路径；禁止堆 VA。
 */
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

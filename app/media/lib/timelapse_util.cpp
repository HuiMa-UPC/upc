#include "timelapse_util.hpp"

#include "device_config.hpp"
#include "print_history_manifest.hpp"
#include "raw_thumb_format.hpp"
#include "rs_log.hpp"

#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
#include <rga/im2d.hpp>
#include <rga/rga.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

namespace upc::media {
namespace timelapse_util {

namespace {

#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
constexpr int kThumbSrcFormat = RK_FORMAT_YCbCr_420_SP;
constexpr int kThumbDstFormat = RK_FORMAT_RGB_565;

bool rga_status_ok(IM_STATUS st)
{
    return st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR;
}
#endif

uint32_t clamp_thumb_width(uint32_t src_w)
{
    const uint32_t max_w = CAMERA_PRINT_VIDEO_THUMB_MAX_W > 0
        ? static_cast<uint32_t>(CAMERA_PRINT_VIDEO_THUMB_MAX_W)
        : src_w;
    return std::max<uint32_t>(1u, std::min(src_w, max_w));
}

uint32_t thumb_height_from_width(uint32_t src_w, uint32_t src_h, uint32_t dst_w)
{
    if(src_w == 0 || src_h == 0 || dst_w == 0) {
        return 1;
    }
    const uint64_t scaled = static_cast<uint64_t>(src_h) * dst_w;
    return std::max<uint32_t>(1u, static_cast<uint32_t>(scaled / src_w));
}

bool write_raw_thumb(const char* out_path,
                     uint32_t width,
                     uint32_t height,
                     uint32_t stride_bytes,
                     const std::vector<uint8_t>& data)
{
    if(!out_path || !out_path[0] || width == 0 || height == 0 ||
       stride_bytes == 0 || data.empty()) {
        return false;
    }

    upc::RawThumbHeader h{};
    std::memcpy(h.magic, upc::kRawThumbMagic, sizeof(h.magic));
    h.version = upc::kRawThumbVersion;
    h.width = width;
    h.height = height;
    h.stride_bytes = stride_bytes;
    h.color_format = static_cast<uint32_t>(upc::RawThumbColorFormat::RGB565);
    h.data_size = static_cast<uint32_t>(data.size());
    if(!upc::raw_thumb_header_valid(h)) {
        return false;
    }

    FILE* fp = std::fopen(out_path, "wb");
    if(!fp) {
        return false;
    }

    bool ok = std::fwrite(&h, 1, sizeof(h), fp) == sizeof(h) &&
              std::fwrite(data.data(), 1, data.size(), fp) == data.size();
    if(std::fclose(fp) != 0) {
        ok = false;
    }
    return ok;
}

} // namespace

std::string sanitize_filename(const char* name)
{
    if(!name || !name[0]) {
        return "print";
    }
    std::string s(name);
    for(char& c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    if(s.empty()) {
        s = "print";
    }
    return s;
}

bool mkdir_p(const char* path)
{
    if(!path || !path[0]) {
        return false;
    }
    char tmp[256];
    std::snprintf(tmp, sizeof(tmp), "%s", path);
    const size_t len = std::strlen(tmp);
    if(len == 0) {
        return false;
    }
    for(size_t i = 1; i < len; ++i) {
        if(tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        if(mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        tmp[i] = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

int run_shell_cmd(const char* cmd, const char* log_tag)
{
    if(!cmd || !cmd[0]) {
        return -1;
    }
    UPC_LOG_MEDIA_SERVICE(Info, "timelapse %s: %s",
                           log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_MEDIA_SERVICE(Error, "timelapse %s failed rc=%d",
                               log_tag ? log_tag : "cmd", rc);
    }
    return rc;
}

std::string frame_png_path(const std::string& session_dir, uint32_t frame_index)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%u.png", frame_index);
    return session_dir + "/" + name;
}

uint32_t scan_max_png_index(const std::string& session_dir)
{
    if(session_dir.empty()) {
        return 0;
    }
    uint32_t max_idx = 0;
    std::error_code ec;
    if(!std::filesystem::is_directory(session_dir, ec)) {
        return 0;
    }
    for(const auto& ent : std::filesystem::directory_iterator(session_dir, ec)) {
        if(ec) {
            break;
        }
        if(!ent.is_regular_file(ec)) {
            continue;
        }
        const std::string name = ent.path().filename().string();
        if(name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0) {
            continue;
        }
        const std::string stem = name.substr(0, name.size() - 4);
        char* end = nullptr;
        const unsigned long v = std::strtoul(stem.c_str(), &end, 10);
        if(!end || *end != '\0' || v == 0 || v > 0xfffffffful) {
            continue;
        }
        max_idx = std::max(max_idx, static_cast<uint32_t>(v));
    }
    return max_idx;
}

bool encode_nv12_to_png(const uint8_t* nv12, size_t bytes,
                        uint32_t width, uint32_t height,
                        const std::string& session_dir, uint32_t frame_index)
{
    if(!nv12 || session_dir.empty() || width == 0 || height == 0 ||
       frame_index == 0) {
        return false;
    }
    const size_t need = static_cast<size_t>(width) * height * 3u / 2u;
    if(bytes < need) {
        return false;
    }

    const std::string png_path = frame_png_path(session_dir, frame_index);
    const std::string tmp_nv12_path = session_dir + "/.tmp_" +
        std::to_string(frame_index) + ".nv12";

    FILE* fp = std::fopen(tmp_nv12_path.c_str(), "wb");
    if(!fp) {
        UPC_LOG_MEDIA_SERVICE(Error, "timelapse open temp nv12 fail %s: %s",
                               tmp_nv12_path.c_str(), std::strerror(errno));
        return false;
    }
    const bool write_ok = std::fwrite(nv12, 1, need, fp) == need;
    if(std::fclose(fp) != 0 || !write_ok) {
        UPC_LOG_MEDIA_SERVICE(Error, "timelapse write temp nv12 fail %s",
                               tmp_nv12_path.c_str());
        (void)std::remove(tmp_nv12_path.c_str());
        return false;
    }

    char cmd[1280];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -i '%s' "
        "-frames:v 1 -c:v png '%s'",
        CAMERA_FFMPEG_BIN, width, height,
        tmp_nv12_path.c_str(), png_path.c_str());
    const bool ok = run_shell_cmd(cmd, "ffmpeg png") == 0;
    (void)std::remove(tmp_nv12_path.c_str());
    if(!ok) {
        (void)std::remove(png_path.c_str());
    }
    return ok;
}

bool encode_png_sequence_to_mp4(const std::string& session_dir,
                                uint32_t frame_count, uint32_t fps,
                                const char* out_path)
{
    if(!out_path || !out_path[0] || session_dir.empty() || frame_count == 0u) {
        return false;
    }
    const uint32_t use_fps = fps > 0 ? fps : 1u;
    char cmd[1536];
    if(frame_count == 1u) {
        std::snprintf(cmd, sizeof(cmd),
                      "%s -y -loglevel error -loop 1 -framerate %u -t 1 "
                      "-i '%s/1.png' -c:v mpeg4 -q:v 5 -pix_fmt yuv420p "
                      "-movflags +faststart '%s'",
                      CAMERA_FFMPEG_BIN, use_fps, session_dir.c_str(), out_path);
    } else {
        std::snprintf(cmd, sizeof(cmd),
                      "%s -y -loglevel error -framerate %u -i '%s/%%d.png' "
                      "-c:v mpeg4 -q:v 5 -pix_fmt yuv420p -movflags +faststart '%s'",
                      CAMERA_FFMPEG_BIN, use_fps, session_dir.c_str(), out_path);
    }
    return run_shell_cmd(cmd, "ffmpeg mp4") == 0;
}

bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                uint32_t width, uint32_t height,
                                const char* out_path)
{
#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
    if(width == 0 || height == 0 || nv12.empty() || !out_path || !out_path[0]) {
        return false;
    }
    const size_t need = static_cast<size_t>(width) * height * 3u / 2u;
    if(nv12.size() < need) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb nv12 too small bytes=%zu need=%zu",
            nv12.size(), need);
        return false;
    }
    if(width > static_cast<uint32_t>(INT_MAX) ||
       height > static_cast<uint32_t>(INT_MAX)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb geometry too large w=%u h=%u", width, height);
        return false;
    }
    const int src_w_i = static_cast<int>(width);
    const int src_h_i = static_cast<int>(height);

    std::vector<uint8_t> rgb565_full(static_cast<size_t>(width) * height * 2u);
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(nv12.data()), src_w_i, src_h_i, kThumbSrcFormat,
        src_w_i, src_h_i);
    rga_buffer_t full_buf = wrapbuffer_virtualaddr(
        rgb565_full.data(), src_w_i, src_h_i, kThumbDstFormat, src_w_i, src_h_i);

    IM_STATUS st = imcvtcolor_t(src_buf, full_buf, kThumbSrcFormat, kThumbDstFormat,
                                IM_COLOR_SPACE_DEFAULT, 1);
    if(!rga_status_ok(st)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb rga cvt failed st=%d (%s)",
                               st, imStrError(st));
        return false;
    }

    const uint32_t dst_w = clamp_thumb_width(width);
    const uint32_t dst_h = thumb_height_from_width(width, height, dst_w);
    const uint32_t dst_stride_px = dst_w;
    const uint32_t dst_stride_bytes = dst_stride_px * 2u;
    if(dst_w > static_cast<uint32_t>(INT_MAX) ||
       dst_h > static_cast<uint32_t>(INT_MAX) ||
       dst_stride_px > static_cast<uint32_t>(INT_MAX)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb dst too large w=%u h=%u stride_px=%u",
            dst_w, dst_h, dst_stride_px);
        return false;
    }
    const int dst_w_i = static_cast<int>(dst_w);
    const int dst_h_i = static_cast<int>(dst_h);
    const int dst_stride_px_i = static_cast<int>(dst_stride_px);
    std::vector<uint8_t> rgb565_thumb(
        static_cast<size_t>(dst_stride_bytes) * dst_h);

    rga_buffer_t in_buf = wrapbuffer_virtualaddr(
        rgb565_full.data(), src_w_i, src_h_i, kThumbDstFormat, src_w_i, src_h_i);
    rga_buffer_t out_buf = wrapbuffer_virtualaddr(
        rgb565_thumb.data(), dst_w_i, dst_h_i, kThumbDstFormat,
        dst_stride_px_i, dst_h_i);
    st = imresize(in_buf, out_buf);
    if(!rga_status_ok(st)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb rga resize failed st=%d (%s)",
                               st, imStrError(st));
        return false;
    }

    if(!write_raw_thumb(out_path, dst_w, dst_h, dst_stride_bytes, rgb565_thumb)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb write failed path=%s", out_path);
        return false;
    }
    return true;
#else
    (void)nv12;
    (void)width;
    (void)height;
    (void)out_path;
    return false;
#endif
}

void remove_png_range(const std::string& session_dir,
                      uint32_t first_index, uint32_t last_index)
{
    if(session_dir.empty() || first_index == 0 || last_index < first_index) {
        return;
    }
    for(uint32_t i = first_index; i <= last_index; ++i) {
        const std::string path = frame_png_path(session_dir, i);
        (void)std::remove(path.c_str());
    }
}

bool remove_empty_dir(const std::string& session_dir)
{
    if(session_dir.empty()) {
        return false;
    }
    std::error_code ec;
    if(!std::filesystem::is_directory(session_dir, ec)) {
        return false;
    }
    if(!std::filesystem::is_empty(session_dir, ec)) {
        return false;
    }
    return std::filesystem::remove(session_dir, ec);
}

bool update_history_media_paths(const std::string& frames_dir,
                                const std::string& thumb_rel,
                                const std::string& video_rel)
{
    if(frames_dir.empty()) {
        return false;
    }
    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_history load fail path=%s", UPC_PRINT_VIDEO_MANIFEST);
        return false;
    }

    bool found = false;
    for(auto& e : entries) {
        if(e.frames_dir == frames_dir) {
            if(!thumb_rel.empty()) {
                e.thumb = thumb_rel;
            }
            if(!video_rel.empty()) {
                e.video = video_rel;
            }
            e.video_pending = false;
            found = true;
            break;
        }
    }
    if(!found) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_history update miss frames_dir=%s", frames_dir.c_str());
        return false;
    }

    const std::string tmp_path =
        std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries)) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_history save fail tmp=%s", tmp_path.c_str());
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    if(std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_history rename fail %s -> %s: %s",
            tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST, std::strerror(errno));
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

} // namespace timelapse_util
} // namespace upc::media

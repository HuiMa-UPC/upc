#include "timelapse_util.hpp"

#include "device_config.hpp"
#include "dma_heap_buffer.hpp"
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
#include <sys/ioctl.h>
#include <sys/stat.h>

#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
#include <linux/dma-buf.h>
#endif

namespace upc::media {
namespace timelapse_util {

namespace {

#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
constexpr int kThumbSrcFormat = RK_FORMAT_YCbCr_420_SP;
/* 实测多工具链下此口径可与 UI 侧 RGB565 解码显示颜色一致。 */
constexpr int kThumbDstFormat = RK_FORMAT_BGR_565;

bool rga_status_ok(IM_STATUS st)
{
    return st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR;
}

void dma_buf_sync(int fd, uint64_t flags)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = flags;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void log_dma_heap_once(const char* heap_path)
{
    static bool logged = false;
    if(logged) {
        return;
    }
    logged = true;
    UPC_LOG_MEDIA_SERVICE(Info, "raw thumb RGA dma heap using %s",
                          heap_path ? heap_path : "?");
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
    char cmd[1280];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -i - "
        "-frames:v 1 -c:v png '%s'",
        CAMERA_FFMPEG_BIN, width, height, png_path.c_str());
    FILE* ff = ::popen(cmd, "w");
    if(!ff) {
        UPC_LOG_MEDIA_SERVICE(Error, "timelapse open ffmpeg pipe fail frame=%u",
                               frame_index);
        return false;
    }
    const bool write_ok = (std::fwrite(nv12, 1, need, ff) == need);
    const int rc = ::pclose(ff);
    const bool ok = write_ok && (rc == 0);
    if(!ok) {
        if(!write_ok) {
            UPC_LOG_MEDIA_SERVICE(Error, "timelapse write frame to ffmpeg fail frame=%u",
                                   frame_index);
        } else {
            UPC_LOG_MEDIA_SERVICE(Error, "timelapse ffmpeg png failed rc=%d frame=%u",
                                   rc, frame_index);
        }
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

bool remux_h264_to_mp4(const char* h264_path, uint32_t fps, const char* out_path)
{
    if(!h264_path || !h264_path[0] || !out_path || !out_path[0]) {
        return false;
    }
    const uint32_t use_fps = fps > 0 ? fps : 1u;
    char cmd[1536];
    std::snprintf(cmd, sizeof(cmd),
                  "%s -y -loglevel error -fflags +genpts -framerate %u "
                  "-i '%s' -c:v copy -movflags +faststart '%s'",
                  CAMERA_FFMPEG_BIN, use_fps, h264_path, out_path);
    return run_shell_cmd(cmd, "ffmpeg h264->mp4") == 0;
}

bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                uint32_t width, uint32_t height,
                                const char* out_path)
{
#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
    /*
     * 与 UI 预览一致：dma32 heap + importbuffer_fd。
     * 禁止 wrapbuffer_virtualaddr(std::vector)：RGA2 无法映射 >4G 普通堆。
     */
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

#if defined(UPC_MEDIA_PHX_RTHM_STREAMING) && UPC_MEDIA_PHX_RTHM_STREAMING
    /*
     * PHX 专用：避免整帧 RGB565 中间缓冲（width*height*2）。
     * 按目标缩略图分块，每块只申请“小块 NV12 + 小块 RGB565”DMA，降低 import 失败概率。
     */
    constexpr uint32_t kDstTileRows = 64u;
    auto align_down_even = [](uint32_t v) { return v & ~1u; };
    auto align_up_even = [](uint32_t v) { return (v + 1u) & ~1u; };

    const uint32_t dst_tile_rows = std::min<uint32_t>(kDstTileRows, dst_h);
    uint32_t max_src_tile_rows = static_cast<uint32_t>(
        (static_cast<uint64_t>(height) * dst_tile_rows + dst_h - 1u) / dst_h);
    max_src_tile_rows = align_up_even(max_src_tile_rows + 2u); // 留一点边界余量
    const uint32_t max_even_height = align_down_even(height);
    if(max_even_height < 2u) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb invalid even source height=%u", height);
        return false;
    }
    if(max_src_tile_rows > max_even_height) {
        max_src_tile_rows = max_even_height;
    }
    if(max_src_tile_rows < 2u) {
        max_src_tile_rows = 2u;
    }

    const size_t src_tile_bytes =
        static_cast<size_t>(width) * max_src_tile_rows * 3u / 2u;
    const size_t rgb_tile_bytes =
        static_cast<size_t>(width) * max_src_tile_rows * 2u;
    const size_t dst_tile_bytes =
        static_cast<size_t>(dst_stride_bytes) * dst_tile_rows;
    const int src_w_i = static_cast<int>(width);
    const int dst_w_i = static_cast<int>(dst_w);
    const int dst_stride_px_i = static_cast<int>(dst_stride_px);

    upc_ipc::DmaHeapBuffer src_tile_dma{};
    upc_ipc::DmaHeapBuffer rgb_tile_dma{};
    upc_ipc::DmaHeapBuffer dst_tile_dma{};
    auto free_dmas = [&]() {
        upc_ipc::dma_heap_free(src_tile_dma);
        upc_ipc::dma_heap_free(rgb_tile_dma);
        upc_ipc::dma_heap_free(dst_tile_dma);
    };

    if(!upc_ipc::dma_heap_alloc(src_tile_bytes, src_tile_dma) ||
       !upc_ipc::dma_heap_alloc(rgb_tile_bytes, rgb_tile_dma) ||
       !upc_ipc::dma_heap_alloc(dst_tile_bytes, dst_tile_dma)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb tile dma_heap alloc failed src_tile=%zu rgb_tile=%zu dst_tile=%zu",
            src_tile_bytes, rgb_tile_bytes, dst_tile_bytes);
        free_dmas();
        return false;
    }
    log_dma_heap_once(src_tile_dma.heap_path);

    rga_buffer_handle_t src_h =
        importbuffer_fd(src_tile_dma.fd, static_cast<int>(src_tile_bytes));
    rga_buffer_handle_t rgb_hnd =
        importbuffer_fd(rgb_tile_dma.fd, static_cast<int>(rgb_tile_bytes));
    rga_buffer_handle_t dst_hnd =
        importbuffer_fd(dst_tile_dma.fd, static_cast<int>(dst_tile_bytes));
    if(src_h == 0 || rgb_hnd == 0 || dst_hnd == 0) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb importbuffer_fd failed src=%llu rgb=%llu dst=%llu",
            static_cast<unsigned long long>(src_h),
            static_cast<unsigned long long>(rgb_hnd),
            static_cast<unsigned long long>(dst_hnd));
        if(src_h) {
            releasebuffer_handle(src_h);
        }
        if(rgb_hnd) {
            releasebuffer_handle(rgb_hnd);
        }
        if(dst_hnd) {
            releasebuffer_handle(dst_hnd);
        }
        free_dmas();
        return false;
    }

    std::vector<uint8_t> rgb565_thumb(
        static_cast<size_t>(dst_stride_bytes) * dst_h, 0u);
    const uint8_t* src_y_base = nv12.data();
    const uint8_t* src_uv_base = src_y_base + static_cast<size_t>(width) * height;

    for(uint32_t dy = 0; dy < dst_h; dy += dst_tile_rows) {
        const uint32_t cur_dst_rows = std::min<uint32_t>(dst_tile_rows, dst_h - dy);
        uint32_t src_y0 = static_cast<uint32_t>(
            (static_cast<uint64_t>(dy) * height) / dst_h);
        uint32_t src_y1 = static_cast<uint32_t>(
            (static_cast<uint64_t>(dy + cur_dst_rows) * height + dst_h - 1u) / dst_h);
        src_y0 = align_down_even(src_y0);
        src_y1 = align_up_even(src_y1);
        if(src_y1 > max_even_height) {
            src_y1 = max_even_height;
        }
        if(src_y1 <= src_y0) {
            src_y1 = std::min<uint32_t>(max_even_height, src_y0 + 2u);
        }
        uint32_t cur_src_rows = src_y1 - src_y0;
        if((cur_src_rows & 1u) != 0u) {
            ++cur_src_rows;
        }
        if(cur_src_rows > max_src_tile_rows) {
            cur_src_rows = max_src_tile_rows;
        }
        if(cur_src_rows < 2u) {
            cur_src_rows = 2u;
        }

        dma_buf_sync(src_tile_dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        uint8_t* tile_y = static_cast<uint8_t*>(src_tile_dma.va);
        uint8_t* tile_uv = tile_y + static_cast<size_t>(width) * cur_src_rows;
        for(uint32_t r = 0; r < cur_src_rows; ++r) {
            const uint8_t* src_row =
                src_y_base + static_cast<size_t>(src_y0 + r) * width;
            std::memcpy(tile_y + static_cast<size_t>(r) * width, src_row, width);
        }
        const uint32_t src_uv_row0 = src_y0 / 2u;
        for(uint32_t r = 0; r < cur_src_rows / 2u; ++r) {
            const uint8_t* src_uv_row =
                src_uv_base + static_cast<size_t>(src_uv_row0 + r) * width;
            std::memcpy(tile_uv + static_cast<size_t>(r) * width, src_uv_row, width);
        }
        dma_buf_sync(src_tile_dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

        rga_buffer_t src_buf = wrapbuffer_handle(
            src_h, src_w_i, static_cast<int>(cur_src_rows),
            kThumbSrcFormat, src_w_i, static_cast<int>(cur_src_rows));
        rga_buffer_t rgb_buf = wrapbuffer_handle(
            rgb_hnd, src_w_i, static_cast<int>(cur_src_rows),
            kThumbDstFormat, src_w_i, static_cast<int>(cur_src_rows));
        rga_buffer_t dst_buf = wrapbuffer_handle(
            dst_hnd, dst_w_i, static_cast<int>(cur_dst_rows),
            kThumbDstFormat, dst_stride_px_i, static_cast<int>(cur_dst_rows));

        IM_STATUS st = imcvtcolor_t(
            src_buf, rgb_buf, kThumbSrcFormat, kThumbDstFormat,
            IM_COLOR_SPACE_DEFAULT, 1);
        if(!rga_status_ok(st)) {
            st = imcvtcolor_t(
                src_buf, rgb_buf, kThumbSrcFormat, kThumbDstFormat,
                IM_YUV_TO_RGB_BT601_LIMIT, 1);
        }
        if(!rga_status_ok(st)) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "raw thumb rga tile csc failed st=%d (%s) dy=%u dst_rows=%u src_rows=%u",
                st, imStrError(st),
                static_cast<unsigned>(dy),
                static_cast<unsigned>(cur_dst_rows),
                static_cast<unsigned>(cur_src_rows));
            releasebuffer_handle(src_h);
            releasebuffer_handle(rgb_hnd);
            releasebuffer_handle(dst_hnd);
            free_dmas();
            return false;
        }

        st = imresize(rgb_buf, dst_buf);
        if(!rga_status_ok(st)) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "raw thumb rga tile resize failed st=%d (%s) dy=%u dst_rows=%u src_rows=%u",
                st, imStrError(st),
                static_cast<unsigned>(dy),
                static_cast<unsigned>(cur_dst_rows),
                static_cast<unsigned>(cur_src_rows));
            releasebuffer_handle(src_h);
            releasebuffer_handle(rgb_hnd);
            releasebuffer_handle(dst_hnd);
            free_dmas();
            return false;
        }

        static bool s_logged_tile_two_step = false;
        if(!s_logged_tile_two_step) {
            s_logged_tile_two_step = true;
            UPC_LOG_MEDIA_SERVICE(Info,
                "raw thumb PHX tile pipeline=TWO_STEP_CSC_RESIZE csc=DEFAULT_THEN_BT601_LIMIT");
        }

        dma_buf_sync(dst_tile_dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        const uint8_t* dst_tile_ptr = static_cast<const uint8_t*>(dst_tile_dma.va);
        for(uint32_t r = 0; r < cur_dst_rows; ++r) {
            uint8_t* out_row =
                rgb565_thumb.data() + static_cast<size_t>(dy + r) * dst_stride_bytes;
            const uint8_t* tile_row = dst_tile_ptr + static_cast<size_t>(r) * dst_stride_bytes;
            std::memcpy(out_row, tile_row, dst_stride_bytes);
        }
        dma_buf_sync(dst_tile_dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }

    releasebuffer_handle(src_h);
    releasebuffer_handle(rgb_hnd);
    releasebuffer_handle(dst_hnd);
    free_dmas();

    if(!write_raw_thumb(out_path, dst_w, dst_h, dst_stride_bytes, rgb565_thumb)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb write failed path=%s", out_path);
        return false;
    }
    return true;
#else
    const size_t full_bytes = static_cast<size_t>(width) * height * 2u;
    const size_t thumb_bytes = static_cast<size_t>(dst_stride_bytes) * dst_h;
    const int src_w_i = static_cast<int>(width);
    const int src_h_i = static_cast<int>(height);
    const int dst_w_i = static_cast<int>(dst_w);
    const int dst_h_i = static_cast<int>(dst_h);
    const int dst_stride_px_i = static_cast<int>(dst_stride_px);

    upc_ipc::DmaHeapBuffer src_dma{};
    upc_ipc::DmaHeapBuffer full_dma{};
    upc_ipc::DmaHeapBuffer thumb_dma{};
    auto free_dmas = [&]() {
        upc_ipc::dma_heap_free(src_dma);
        upc_ipc::dma_heap_free(full_dma);
        upc_ipc::dma_heap_free(thumb_dma);
    };

    if(!upc_ipc::dma_heap_alloc(need, src_dma) ||
       !upc_ipc::dma_heap_alloc(full_bytes, full_dma) ||
       !upc_ipc::dma_heap_alloc(thumb_bytes, thumb_dma)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb dma_heap alloc failed nv12=%zu full=%zu thumb=%zu",
            need, full_bytes, thumb_bytes);
        free_dmas();
        return false;
    }
    log_dma_heap_once(src_dma.heap_path);

    dma_buf_sync(src_dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memcpy(src_dma.va, nv12.data(), need);
    dma_buf_sync(src_dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    rga_buffer_handle_t src_h = importbuffer_fd(src_dma.fd, static_cast<int>(need));
    rga_buffer_handle_t full_h =
        importbuffer_fd(full_dma.fd, static_cast<int>(full_bytes));
    rga_buffer_handle_t thumb_h =
        importbuffer_fd(thumb_dma.fd, static_cast<int>(thumb_bytes));
    if(src_h == 0 || full_h == 0 || thumb_h == 0) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "raw thumb importbuffer_fd failed src=%llu full=%llu thumb=%llu",
            static_cast<unsigned long long>(src_h),
            static_cast<unsigned long long>(full_h),
            static_cast<unsigned long long>(thumb_h));
        if(src_h) {
            releasebuffer_handle(src_h);
        }
        if(full_h) {
            releasebuffer_handle(full_h);
        }
        if(thumb_h) {
            releasebuffer_handle(thumb_h);
        }
        free_dmas();
        return false;
    }

    rga_buffer_t src_buf = wrapbuffer_handle(
        src_h, src_w_i, src_h_i, kThumbSrcFormat, src_w_i, src_h_i);
    rga_buffer_t full_buf = wrapbuffer_handle(
        full_h, src_w_i, src_h_i, kThumbDstFormat, src_w_i, src_h_i);
    rga_buffer_t thumb_buf = wrapbuffer_handle(
        thumb_h, dst_w_i, dst_h_i, kThumbDstFormat, dst_stride_px_i, dst_h_i);

    const im_rect full_rect{0, 0, src_w_i, src_h_i};
    IM_STATUS st = imcheck(src_buf, full_buf, full_rect, full_rect);
    if(!rga_status_ok(st)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb rga cvt imcheck failed st=%d (%s)",
                              st, imStrError(st));
        releasebuffer_handle(src_h);
        releasebuffer_handle(full_h);
        releasebuffer_handle(thumb_h);
        free_dmas();
        return false;
    }

    st = imcvtcolor_t(src_buf, full_buf, kThumbSrcFormat, kThumbDstFormat,
                      IM_COLOR_SPACE_DEFAULT, 1);
    if(!rga_status_ok(st)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb rga cvt failed st=%d (%s)",
                              st, imStrError(st));
        releasebuffer_handle(src_h);
        releasebuffer_handle(full_h);
        releasebuffer_handle(thumb_h);
        free_dmas();
        return false;
    }

    st = imresize(full_buf, thumb_buf);
    if(!rga_status_ok(st)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb rga resize failed st=%d (%s)",
                              st, imStrError(st));
        releasebuffer_handle(src_h);
        releasebuffer_handle(full_h);
        releasebuffer_handle(thumb_h);
        free_dmas();
        return false;
    }

    releasebuffer_handle(src_h);
    releasebuffer_handle(full_h);
    releasebuffer_handle(thumb_h);

    dma_buf_sync(thumb_dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    std::vector<uint8_t> rgb565_thumb(thumb_bytes);
    std::memcpy(rgb565_thumb.data(), thumb_dma.va, thumb_bytes);
    dma_buf_sync(thumb_dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    free_dmas();

    if(!write_raw_thumb(out_path, dst_w, dst_h, dst_stride_bytes, rgb565_thumb)) {
        UPC_LOG_MEDIA_SERVICE(Warn, "raw thumb write failed path=%s", out_path);
        return false;
    }
    return true;
#endif
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

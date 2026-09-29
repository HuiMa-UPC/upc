#include "transfer_thumbnail_generator.hpp"

#include "raw_thumb_format.hpp"
#include "rs_log.hpp"

#include <png.h>

#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
#include "dma_heap_buffer.hpp"
#include <linux/dma-buf.h>
#include <rga/im2d.hpp>
#include <rga/rga.h>
#include <sys/ioctl.h>
#endif

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace upc::xfr {

namespace {

constexpr uint32_t kMaxPngDimension = 8192;
constexpr uint64_t kMaxPngPixels = 32ULL * 1024ULL * 1024ULL;
/** Snowpath 预览四周大量留白；先抽到此长边，再抠内容 COVER，模型区仍够 320。 */
constexpr uint32_t kDecodeMaxEdge = 1280;
constexpr int kBgColorTol = 18;
constexpr int kBgAlphaTol = 16;
constexpr int kCornerAgreeTol = 24;
constexpr uint32_t kMinContentEdge = 8;

struct ContentBox {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t bg_r = 255;
    uint8_t bg_g = 255;
    uint8_t bg_b = 255;
};

struct DecodedPng {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

bool decode_png_rgba(const std::string& path, uint32_t max_edge,
                     DecodedPng* decoded, std::string* error)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if(!fp) {
        *error = "cannot open extracted PNG";
        return false;
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if(!png) {
        std::fclose(fp);
        *error = "cannot create PNG decoder";
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if(!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        *error = "cannot create PNG metadata";
        return false;
    }
    if(setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        *error = "invalid or unsupported PNG";
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);
    const uint32_t width = png_get_image_width(png, info);
    const uint32_t height = png_get_image_height(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);
    if(width == 0 || height == 0 ||
       width > kMaxPngDimension || height > kMaxPngDimension ||
       static_cast<uint64_t>(width) * height > kMaxPngPixels) {
        png_error(png, "PNG dimensions exceed transfer thumbnail limit");
    }
    if(bit_depth == 16) {
        png_set_strip_16(png);
    }
    if(color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if(png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if(color_type == PNG_COLOR_TYPE_RGB ||
       color_type == PNG_COLOR_TYPE_GRAY ||
       color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY ||
       color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);
    if(png_get_rowbytes(png, info) != static_cast<png_size_t>(width) * 4u) {
        png_error(png, "unexpected decoded PNG row size");
    }

    uint32_t step = 1;
    const uint32_t long_edge = std::max(width, height);
    const uint32_t decode_edge = std::max(1u, max_edge);
    if(long_edge > decode_edge) {
        step = (long_edge + decode_edge - 1u) / decode_edge;
    }
    const uint32_t out_w = (width + step - 1u) / step;
    const uint32_t out_h = (height + step - 1u) / step;
    decoded->rgba.assign(static_cast<size_t>(out_w) * out_h * 4u, 0);

    if(step == 1u) {
        for(uint32_t y = 0; y < height; ++y) {
            png_bytep row =
                decoded->rgba.data() + static_cast<size_t>(y) * width * 4u;
            png_read_row(png, row, nullptr);
        }
    } else {
        std::vector<uint8_t> row(static_cast<size_t>(width) * 4u);
        uint32_t oy = 0;
        for(uint32_t y = 0; y < height; ++y) {
            png_read_row(png, row.data(), nullptr);
            if((y % step) != 0u) {
                continue;
            }
            uint8_t* dst = decoded->rgba.data() +
                           static_cast<size_t>(oy) * out_w * 4u;
            for(uint32_t ox = 0; ox < out_w; ++ox) {
                const uint32_t sx = std::min(ox * step, width - 1u);
                std::memcpy(dst + static_cast<size_t>(ox) * 4u,
                            row.data() + static_cast<size_t>(sx) * 4u, 4u);
            }
            ++oy;
        }
        UPC_LOG_XFR_SERVICE(Info,
            "3mf thumbnail decode downsample %ux%u -> %ux%u step=%u",
            static_cast<unsigned>(width), static_cast<unsigned>(height),
            static_cast<unsigned>(out_w), static_cast<unsigned>(out_h),
            static_cast<unsigned>(step));
    }
    png_read_end(png, nullptr);
    decoded->width = out_w;
    decoded->height = out_h;
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return true;
}

/** 把 src 整框按比例放进 dst，多出来的边稍后用底色填。 */
void contain_dest(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                  uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h)
{
    if(static_cast<uint64_t>(src_w) * dst_h >
       static_cast<uint64_t>(src_h) * dst_w) {
        *w = dst_w;
        *h = static_cast<uint32_t>(
            std::max<uint64_t>(1, static_cast<uint64_t>(dst_w) * src_h / src_w));
        *x = 0;
        *y = (dst_h - *h) / 2u;
    } else {
        *h = dst_h;
        *w = static_cast<uint32_t>(
            std::max<uint64_t>(1, static_cast<uint64_t>(dst_h) * src_w / src_h));
        *x = (dst_w - *w) / 2u;
        *y = 0;
    }
}

uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

void fill_rgb565(std::vector<uint8_t>* rgb565, uint32_t w, uint32_t h, uint16_t pixel)
{
    rgb565->assign(static_cast<size_t>(w) * h * 2u, 0);
    const uint8_t lo = static_cast<uint8_t>(pixel & 0xffu);
    const uint8_t hi = static_cast<uint8_t>(pixel >> 8);
    for(size_t i = 0; i < rgb565->size(); i += 2u) {
        (*rgb565)[i] = lo;
        (*rgb565)[i + 1u] = hi;
    }
}

void blit_rgb565(std::vector<uint8_t>* dst, uint32_t dst_w, uint32_t dst_h,
                 const std::vector<uint8_t>& src, uint32_t src_w, uint32_t src_h,
                 uint32_t x, uint32_t y)
{
    if(x >= dst_w || y >= dst_h || src_w == 0 || src_h == 0) {
        return;
    }
    const uint32_t copy_w = std::min(src_w, dst_w - x);
    const uint32_t copy_h = std::min(src_h, dst_h - y);
    for(uint32_t row = 0; row < copy_h; ++row) {
        std::memcpy(dst->data() + (static_cast<size_t>(y + row) * dst_w + x) * 2u,
                    src.data() + static_cast<size_t>(row) * src_w * 2u,
                    static_cast<size_t>(copy_w) * 2u);
    }
}

bool pixel_is_background(const uint8_t* p, const uint8_t* bg)
{
    if(bg[3] <= kBgAlphaTol) {
        return p[3] <= kBgAlphaTol;
    }
    if(p[3] <= kBgAlphaTol) {
        return true;
    }
    return std::abs(static_cast<int>(p[0]) - static_cast<int>(bg[0])) <= kBgColorTol &&
           std::abs(static_cast<int>(p[1]) - static_cast<int>(bg[1])) <= kBgColorTol &&
           std::abs(static_cast<int>(p[2]) - static_cast<int>(bg[2])) <= kBgColorTol;
}

/**
 * 旧版 Snowpath 预览用青绿/青色画支撑柱，模型和浅灰底都不是这个色。
 * 新版蓝底预览里支撑和模型同是暗蓝，B 明显高于 G，这里故意不匹配，避免把莲花抹掉。
 */
bool is_cyan_support(const uint8_t* p)
{
    const int r = p[0];
    const int g = p[1];
    const int b = p[2];
    const int a = p[3];
    if(a <= kBgAlphaTol) {
        return false;
    }
    const int mx = std::max(r, std::max(g, b));
    const int mn = std::min(r, std::min(g, b));
    if(mx < 40) {
        return false;
    }
    if((mx - mn) * 255 / mx < 40) {
        return false;
    }
    if(r >= g || r >= b || g < 50) {
        return false;
    }
    if(std::abs(g - b) > 40) {
        return false;
    }
    if(b > g + 30) {
        return false;
    }
    return true;
}

bool sample_background(const DecodedPng& src, uint8_t bg[4])
{
    const uint8_t* corners[4] = {
        src.rgba.data(),
        src.rgba.data() + static_cast<size_t>(src.width - 1u) * 4u,
        src.rgba.data() + static_cast<size_t>(src.height - 1u) * src.width * 4u,
        src.rgba.data() +
            (static_cast<size_t>(src.height - 1u) * src.width + (src.width - 1u)) * 4u};
    int acc[4] = {0, 0, 0, 0};
    for(const uint8_t* c : corners) {
        acc[0] += c[0];
        acc[1] += c[1];
        acc[2] += c[2];
        acc[3] += c[3];
    }
    bg[0] = static_cast<uint8_t>(acc[0] / 4);
    bg[1] = static_cast<uint8_t>(acc[1] / 4);
    bg[2] = static_cast<uint8_t>(acc[2] / 4);
    bg[3] = static_cast<uint8_t>(acc[3] / 4);
    for(const uint8_t* c : corners) {
        if(std::abs(static_cast<int>(c[0]) - static_cast<int>(bg[0])) > kCornerAgreeTol ||
           std::abs(static_cast<int>(c[1]) - static_cast<int>(bg[1])) > kCornerAgreeTol ||
           std::abs(static_cast<int>(c[2]) - static_cast<int>(bg[2])) > kCornerAgreeTol ||
           std::abs(static_cast<int>(c[3]) - static_cast<int>(bg[3])) > kCornerAgreeTol) {
            return false;
        }
    }
    return true;
}

uint32_t erase_support_tint(DecodedPng* src, const uint8_t bg[4])
{
    uint32_t erased = 0;
    const size_t pixels = static_cast<size_t>(src->width) * src->height;
    for(size_t i = 0; i < pixels; ++i) {
        uint8_t* p = src->rgba.data() + i * 4u;
        if(pixel_is_background(p, bg) || !is_cyan_support(p)) {
            continue;
        }
        p[0] = bg[0];
        p[1] = bg[1];
        p[2] = bg[2];
        p[3] = bg[3] != 0 ? bg[3] : 255;
        ++erased;
    }
    return erased;
}

ContentBox make_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t bg[4])
{
    return ContentBox{x, y, w, h, bg[0], bg[1], bg[2]};
}

ContentBox full_box(const DecodedPng& src, const uint8_t bg[4])
{
    return make_box(0, 0, src.width, src.height, bg);
}

/**
 * Snowpath Thumbnails/*.png 把模型画在大画布正中，四周是均匀底色或透明。
 * 四角颜色一致才抠内容；检测失败或内容过小则退回整图，避免误裁。
 */
ContentBox find_content_box(const DecodedPng& src)
{
    const uint8_t fallback_bg[4] = {255, 255, 255, 255};
    if(src.width == 0 || src.height == 0 ||
       src.rgba.size() < static_cast<size_t>(src.width) * src.height * 4u) {
        return full_box(src, fallback_bg);
    }

    uint8_t bg[4] = {255, 255, 255, 255};
    if(!sample_background(src, bg)) {
        return full_box(src, bg);
    }

    uint32_t minx = src.width;
    uint32_t miny = src.height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    bool found = false;
    for(uint32_t y = 0; y < src.height; ++y) {
        const uint8_t* row =
            src.rgba.data() + static_cast<size_t>(y) * src.width * 4u;
        for(uint32_t x = 0; x < src.width; ++x) {
            if(pixel_is_background(row + static_cast<size_t>(x) * 4u, bg)) {
                continue;
            }
            found = true;
            if(x < minx) {
                minx = x;
            }
            if(y < miny) {
                miny = y;
            }
            if(x > maxx) {
                maxx = x;
            }
            if(y > maxy) {
                maxy = y;
            }
        }
    }
    if(!found || maxx < minx || maxy < miny) {
        return full_box(src, bg);
    }

    const uint32_t raw_w = maxx - minx + 1u;
    const uint32_t raw_h = maxy - miny + 1u;
    if(raw_w < kMinContentEdge || raw_h < kMinContentEdge) {
        return full_box(src, bg);
    }

    const uint32_t pad = std::max(4u, std::max(raw_w, raw_h) / 10u);
    const uint32_t x = minx > pad ? minx - pad : 0u;
    const uint32_t y = miny > pad ? miny - pad : 0u;
    const uint32_t x2 = std::min(src.width, maxx + 1u + pad);
    const uint32_t y2 = std::min(src.height, maxy + 1u + pad);
    return make_box(x, y, x2 - x, y2 - y, bg);
}

#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
constexpr int kRgaSrcFormat = RK_FORMAT_RGBA_8888;
constexpr int kRgaDstFormat = RK_FORMAT_RGB_565;
/** RGA2 单趟缩放区间 1/8～8；72×72 相对 2048 源要先落到中间尺寸。 */
constexpr uint32_t kRgaMaxScale = 8;

bool rga_status_ok(IM_STATUS status)
{
    return status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
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

void align_even_rect(uint32_t src_w, uint32_t src_h,
                     uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h)
{
    *x &= ~1u;
    *y &= ~1u;
    *w = (*w + 1u) & ~1u;
    *h = (*h + 1u) & ~1u;
    if(*x + *w > src_w) {
        *w = (src_w - *x) & ~1u;
    }
    if(*y + *h > src_h) {
        *h = (src_h - *y) & ~1u;
    }
}

bool rga_process(rga_buffer_handle_t src_handle, int src_w, int src_hgt,
                 int src_fmt, const im_rect& src_rect,
                 rga_buffer_handle_t dst_handle, int dst_w, int dst_hgt,
                 int dst_fmt)
{
    rga_buffer_t src = wrapbuffer_handle(
        src_handle, src_w, src_hgt, src_fmt, src_w, src_hgt);
    rga_buffer_t dst = wrapbuffer_handle(
        dst_handle, dst_w, dst_hgt, dst_fmt, dst_w, dst_hgt);
    rga_buffer_t pat{};
    const im_rect dst_rect{0, 0, dst_w, dst_hgt};
    const im_rect pat_rect{};
    IM_STATUS st = imcheck(src, dst, src_rect, dst_rect);
    if(!rga_status_ok(st)) {
        UPC_LOG_XFR_SERVICE(Warn,
            "3mf thumbnail rga imcheck failed st=%d (%s) src=%dx%d crop=%d,%d %dx%d dst=%dx%d",
            st, imStrError(st), src_w, src_hgt,
            src_rect.x, src_rect.y, src_rect.width, src_rect.height,
            dst_w, dst_hgt);
        return false;
    }
    st = improcess(src, dst, pat, src_rect, dst_rect, pat_rect, IM_SYNC);
    if(!rga_status_ok(st)) {
        UPC_LOG_XFR_SERVICE(Warn,
            "3mf thumbnail rga improcess failed st=%d (%s) src=%dx%d dst=%dx%d",
            st, imStrError(st), src_w, src_hgt, dst_w, dst_hgt);
        return false;
    }
    return true;
}

/**
 * 抽点后的 RGBA 拷进 dma32，用 importbuffer_fd 交给 RGA。
 * 禁止 wrapbuffer_virtualaddr：RGA2 无 IOMMU，普通堆虚地址会整进程退出。
 */
struct RgaThumbSource {
    upc_ipc::DmaHeapBuffer dma{};
    rga_buffer_handle_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    ~RgaThumbSource()
    {
        close();
    }

    void close()
    {
        if(handle != 0) {
            releasebuffer_handle(handle);
            handle = 0;
        }
        upc_ipc::dma_heap_free(dma);
        width = 0;
        height = 0;
    }

    bool import(const DecodedPng& src)
    {
        close();
        if(src.width == 0 || src.height == 0 ||
           src.width > static_cast<uint32_t>(INT_MAX) ||
           src.height > static_cast<uint32_t>(INT_MAX)) {
            return false;
        }
        const size_t bytes = static_cast<size_t>(src.width) * src.height * 4u;
        if(src.rgba.size() < bytes) {
            return false;
        }
        if(!upc_ipc::dma_heap_alloc(bytes, dma, upc_ipc::DmaHeapUsage::RgaDevice)) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail rga src dma_heap alloc failed bytes=%zu (no RGA-safe heap)",
                bytes);
            return false;
        }
        dma_buf_sync(dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        std::memcpy(dma.va, src.rgba.data(), bytes);
        dma_buf_sync(dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

        handle = importbuffer_fd(dma.fd, static_cast<int>(bytes));
        if(handle == 0) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail importbuffer_fd failed bytes=%zu heap=%s",
                bytes, dma.heap_path ? dma.heap_path : "?");
            close();
            return false;
        }
        width = src.width;
        height = src.height;
        UPC_LOG_XFR_SERVICE(Info,
            "3mf thumbnail RGA src %ux%u heap=%s",
            static_cast<unsigned>(width), static_cast<unsigned>(height),
            dma.heap_path ? dma.heap_path : "?");
        return true;
    }

    bool render(uint32_t dst_w, uint32_t dst_h, const ContentBox& content,
                std::vector<uint8_t>* rgb565)
    {
        if(handle == 0 || dst_w == 0 || dst_h == 0 ||
           dst_w > static_cast<uint32_t>(INT_MAX) ||
           dst_h > static_cast<uint32_t>(INT_MAX) ||
           content.w == 0 || content.h == 0) {
            return false;
        }
        uint32_t crop_x = content.x;
        uint32_t crop_y = content.y;
        uint32_t crop_w = content.w;
        uint32_t crop_h = content.h;
        align_even_rect(width, height, &crop_x, &crop_y, &crop_w, &crop_h);
        if(crop_w < 2u || crop_h < 2u) {
            return false;
        }

        uint32_t fit_x = 0;
        uint32_t fit_y = 0;
        uint32_t fit_w = 0;
        uint32_t fit_h = 0;
        contain_dest(crop_w, crop_h, dst_w, dst_h, &fit_x, &fit_y, &fit_w, &fit_h);
        fit_w = std::max(2u, fit_w & ~1u);
        fit_h = std::max(2u, fit_h & ~1u);
        if(fit_w > dst_w) {
            fit_w = dst_w & ~1u;
        }
        if(fit_h > dst_h) {
            fit_h = dst_h & ~1u;
        }
        if(fit_w < 2u || fit_h < 2u) {
            return false;
        }
        fit_x = (dst_w - fit_w) / 2u;
        fit_y = (dst_h - fit_h) / 2u;

        const size_t fit_bytes = static_cast<size_t>(fit_w) * fit_h * 2u;
        upc_ipc::DmaHeapBuffer dst_dma{};
        if(!upc_ipc::dma_heap_alloc(fit_bytes, dst_dma,
                                    upc_ipc::DmaHeapUsage::RgaDevice)) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail rga dst dma_heap alloc failed bytes=%zu",
                fit_bytes);
            return false;
        }
        rga_buffer_handle_t dst_hnd =
            importbuffer_fd(dst_dma.fd, static_cast<int>(fit_bytes));
        if(dst_hnd == 0) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail dest importbuffer_fd failed bytes=%zu heap=%s",
                fit_bytes, dst_dma.heap_path ? dst_dma.heap_path : "?");
            upc_ipc::dma_heap_free(dst_dma);
            return false;
        }

        const im_rect crop{
            static_cast<int>(crop_x), static_cast<int>(crop_y),
            static_cast<int>(crop_w), static_cast<int>(crop_h)};
        const bool two_pass =
            crop_w > fit_w * kRgaMaxScale || crop_h > fit_h * kRgaMaxScale;
        bool ok = false;
        if(!two_pass) {
            ok = rga_process(handle,
                             static_cast<int>(width), static_cast<int>(height),
                             kRgaSrcFormat, crop,
                             dst_hnd,
                             static_cast<int>(fit_w), static_cast<int>(fit_h),
                             kRgaDstFormat);
        } else {
            uint32_t mid_w = (crop_w + kRgaMaxScale - 1u) / kRgaMaxScale;
            uint32_t mid_h = (crop_h + kRgaMaxScale - 1u) / kRgaMaxScale;
            mid_w = std::max(fit_w, (mid_w + 1u) & ~1u);
            mid_h = std::max(fit_h, (mid_h + 1u) & ~1u);
            const size_t mid_bytes = static_cast<size_t>(mid_w) * mid_h * 2u;
            upc_ipc::DmaHeapBuffer mid_dma{};
            if(!upc_ipc::dma_heap_alloc(mid_bytes, mid_dma,
                                        upc_ipc::DmaHeapUsage::RgaDevice)) {
                UPC_LOG_XFR_SERVICE(Warn,
                    "3mf thumbnail rga mid dma_heap alloc failed bytes=%zu",
                    mid_bytes);
            } else {
                rga_buffer_handle_t mid_hnd =
                    importbuffer_fd(mid_dma.fd, static_cast<int>(mid_bytes));
                if(mid_hnd == 0) {
                    UPC_LOG_XFR_SERVICE(Warn,
                        "3mf thumbnail mid importbuffer_fd failed bytes=%zu heap=%s",
                        mid_bytes, mid_dma.heap_path ? mid_dma.heap_path : "?");
                } else {
                    const im_rect mid_full{
                        0, 0,
                        static_cast<int>(mid_w), static_cast<int>(mid_h)};
                    ok = rga_process(handle,
                                     static_cast<int>(width),
                                     static_cast<int>(height),
                                     kRgaSrcFormat, crop,
                                     mid_hnd,
                                     static_cast<int>(mid_w),
                                     static_cast<int>(mid_h),
                                     kRgaDstFormat) &&
                         rga_process(mid_hnd,
                                     static_cast<int>(mid_w),
                                     static_cast<int>(mid_h),
                                     kRgaDstFormat, mid_full,
                                     dst_hnd,
                                     static_cast<int>(fit_w),
                                     static_cast<int>(fit_h),
                                     kRgaDstFormat);
                    releasebuffer_handle(mid_hnd);
                }
                upc_ipc::dma_heap_free(mid_dma);
            }
        }

        if(ok) {
            dma_buf_sync(dst_dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            std::vector<uint8_t> fitted(
                static_cast<const uint8_t*>(dst_dma.va),
                static_cast<const uint8_t*>(dst_dma.va) + fit_bytes);
            dma_buf_sync(dst_dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            if(fit_w == dst_w && fit_h == dst_h) {
                rgb565->swap(fitted);
            } else {
                fill_rgb565(rgb565, dst_w, dst_h,
                            pack_rgb565(content.bg_r, content.bg_g, content.bg_b));
                blit_rgb565(rgb565, dst_w, dst_h, fitted, fit_w, fit_h, fit_x, fit_y);
            }
        }
        releasebuffer_handle(dst_hnd);
        upc_ipc::dma_heap_free(dst_dma);
        return ok;
    }
};
#endif

void render_cpu(const DecodedPng& src, uint32_t dst_w, uint32_t dst_h,
                const ContentBox& content, std::vector<uint8_t>* rgb565)
{
    uint32_t fit_x = 0;
    uint32_t fit_y = 0;
    uint32_t fit_w = 0;
    uint32_t fit_h = 0;
    contain_dest(content.w, content.h, dst_w, dst_h, &fit_x, &fit_y, &fit_w, &fit_h);
    fill_rgb565(rgb565, dst_w, dst_h,
                pack_rgb565(content.bg_r, content.bg_g, content.bg_b));
    if(fit_w == 0 || fit_h == 0 || content.w == 0 || content.h == 0) {
        return;
    }

    for(uint32_t y = 0; y < fit_h; ++y) {
        const uint32_t dy = fit_y + y;
        const uint64_t fy = ((static_cast<uint64_t>(y) * 2u + 1u) * content.h << 15) /
                            fit_h;
        const uint64_t sy_fp = (static_cast<uint64_t>(content.y) << 16) +
                               (fy > 32768u ? fy - 32768u : 0u);
        const uint32_t sy0 = std::min<uint32_t>(src.height - 1u, sy_fp >> 16);
        const uint32_t sy1 = std::min<uint32_t>(src.height - 1u, sy0 + 1u);
        const uint32_t wy = static_cast<uint32_t>(sy_fp & 0xffffu);
        for(uint32_t x = 0; x < fit_w; ++x) {
            const uint32_t dx = fit_x + x;
            const uint64_t fx = ((static_cast<uint64_t>(x) * 2u + 1u) * content.w << 15) /
                                fit_w;
            const uint64_t sx_fp = (static_cast<uint64_t>(content.x) << 16) +
                                   (fx > 32768u ? fx - 32768u : 0u);
            const uint32_t sx0 = std::min<uint32_t>(src.width - 1u, sx_fp >> 16);
            const uint32_t sx1 = std::min<uint32_t>(src.width - 1u, sx0 + 1u);
            const uint32_t wx = static_cast<uint32_t>(sx_fp & 0xffffu);
            const uint8_t* p00 = src.rgba.data() +
                (static_cast<size_t>(sy0) * src.width + sx0) * 4u;
            const uint8_t* p01 = src.rgba.data() +
                (static_cast<size_t>(sy0) * src.width + sx1) * 4u;
            const uint8_t* p10 = src.rgba.data() +
                (static_cast<size_t>(sy1) * src.width + sx0) * 4u;
            const uint8_t* p11 = src.rgba.data() +
                (static_cast<size_t>(sy1) * src.width + sx1) * 4u;
            const auto channel = [&](size_t c) -> uint8_t {
                const uint64_t top =
                    static_cast<uint64_t>(p00[c]) * (65536u - wx) +
                    static_cast<uint64_t>(p01[c]) * wx;
                const uint64_t bottom =
                    static_cast<uint64_t>(p10[c]) * (65536u - wx) +
                    static_cast<uint64_t>(p11[c]) * wx;
                return static_cast<uint8_t>(
                    ((top * (65536u - wy) + bottom * wy) + (1ULL << 31)) >> 32);
            };
            const uint16_t pixel = pack_rgb565(channel(0), channel(1), channel(2));
            const size_t offset = (static_cast<size_t>(dy) * dst_w + dx) * 2u;
            (*rgb565)[offset] = static_cast<uint8_t>(pixel & 0xffu);
            (*rgb565)[offset + 1] = static_cast<uint8_t>(pixel >> 8);
        }
    }
}

bool write_rthm(const TransferThumbnailTarget& target,
                const std::vector<uint8_t>& pixels,
                std::string* error)
{
    if(target.width == 0 || target.height == 0 ||
       pixels.size() != static_cast<size_t>(target.width) * target.height * 2u) {
        *error = "invalid generated RTHM geometry";
        return false;
    }
    RawThumbHeader header{};
    std::memcpy(header.magic, kRawThumbMagic, sizeof(header.magic));
    header.version = kRawThumbVersion;
    header.width = target.width;
    header.height = target.height;
    header.stride_bytes = target.width * 2u;
    header.color_format = static_cast<uint32_t>(RawThumbColorFormat::RGB565);
    header.data_size = static_cast<uint32_t>(pixels.size());

    std::ofstream out(target.path, std::ios::binary | std::ios::trunc);
    if(!out) {
        *error = "cannot create staged RTHM";
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    out.close();
    if(!out) {
        std::remove(target.path.c_str());
        *error = "cannot write staged RTHM";
        return false;
    }
    return true;
}

} // namespace

bool generate_transfer_thumbnail_triplet(
    const std::string& png_path,
    const std::array<TransferThumbnailTarget, 3>& targets,
    std::string* error)
{
    if(!error) {
        return false;
    }
    uint32_t decode_edge = kDecodeMaxEdge;
    for(const TransferThumbnailTarget& target : targets) {
        decode_edge = std::max(decode_edge, std::max(target.width, target.height));
    }
    if(decode_edge == 0) {
        *error = "invalid thumbnail target geometry";
        return false;
    }

    DecodedPng decoded;
    if(!decode_png_rgba(png_path, decode_edge, &decoded, error)) {
        return false;
    }
    uint8_t canvas_bg[4] = {255, 255, 255, 255};
    if(sample_background(decoded, canvas_bg)) {
        const uint32_t erased = erase_support_tint(&decoded, canvas_bg);
        if(erased > 0u) {
            UPC_LOG_XFR_SERVICE(Info,
                "3mf thumbnail erase support-tint pixels=%u src=%ux%u",
                static_cast<unsigned>(erased),
                static_cast<unsigned>(decoded.width),
                static_cast<unsigned>(decoded.height));
        }
    }
    const ContentBox content = find_content_box(decoded);
    if(content.x != 0u || content.y != 0u ||
       content.w != decoded.width || content.h != decoded.height) {
        UPC_LOG_XFR_SERVICE(Info,
            "3mf thumbnail trim content %ux%u+%u+%u of %ux%u",
            static_cast<unsigned>(content.w), static_cast<unsigned>(content.h),
            static_cast<unsigned>(content.x), static_cast<unsigned>(content.y),
            static_cast<unsigned>(decoded.width),
            static_cast<unsigned>(decoded.height));
    }

    std::vector<std::string> created;
#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
    /* 抽点后的图可以给 RGA，但必须走 dma32 + importbuffer_fd。
     * wrapbuffer_virtualaddr 喂普通堆会整进程退出，与图大不大无关。 */
    RgaThumbSource rga_src;
    const bool rga_ready = rga_src.import(decoded);
    if(!rga_ready) {
        UPC_LOG_XFR_SERVICE(Warn,
            "3mf thumbnail RGA src unavailable, CPU for all sizes png=%s src=%ux%u",
            png_path.c_str(),
            static_cast<unsigned>(decoded.width),
            static_cast<unsigned>(decoded.height));
    }
#else
    UPC_LOG_XFR_SERVICE(Info, "3mf thumbnail CPU png=%s src=%ux%u",
                        png_path.c_str(),
                        static_cast<unsigned>(decoded.width),
                        static_cast<unsigned>(decoded.height));
#endif
    for(const TransferThumbnailTarget& target : targets) {
        std::vector<uint8_t> pixels;
        bool rendered = false;
#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
        if(rga_ready) {
            rendered = rga_src.render(target.width, target.height, content, &pixels);
            if(rendered) {
                UPC_LOG_XFR_SERVICE(Info,
                    "3mf thumbnail RGA dst=%ux%u",
                    static_cast<unsigned>(target.width),
                    static_cast<unsigned>(target.height));
            } else {
                UPC_LOG_XFR_SERVICE(Warn,
                    "3mf thumbnail RGA failed, CPU dst=%ux%u",
                    static_cast<unsigned>(target.width),
                    static_cast<unsigned>(target.height));
            }
        }
#endif
        if(!rendered) {
            render_cpu(decoded, target.width, target.height, content, &pixels);
        }
        if(!write_rthm(target, pixels, error)) {
            for(const std::string& path : created) {
                std::remove(path.c_str());
            }
            return false;
        }
        created.push_back(target.path);
    }
    return true;
}

} // namespace upc::xfr

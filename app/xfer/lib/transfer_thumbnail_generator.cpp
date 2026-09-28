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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace upc::xfr {

namespace {

constexpr uint32_t kMaxPngDimension = 8192;
constexpr uint64_t kMaxPngPixels = 32ULL * 1024ULL * 1024ULL;

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

void cover_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h)
{
    if(static_cast<uint64_t>(src_w) * dst_h >
       static_cast<uint64_t>(src_h) * dst_w) {
        *h = src_h;
        *w = static_cast<uint32_t>(
            std::max<uint64_t>(1, static_cast<uint64_t>(src_h) * dst_w / dst_h));
        *x = (src_w - *w) / 2u;
        *y = 0;
    } else {
        *w = src_w;
        *h = static_cast<uint32_t>(
            std::max<uint64_t>(1, static_cast<uint64_t>(src_w) * dst_h / dst_w));
        *x = 0;
        *y = (src_h - *h) / 2u;
    }
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

    bool render(uint32_t dst_w, uint32_t dst_h, std::vector<uint8_t>* rgb565)
    {
        if(handle == 0 || dst_w == 0 || dst_h == 0 ||
           dst_w > static_cast<uint32_t>(INT_MAX) ||
           dst_h > static_cast<uint32_t>(INT_MAX)) {
            return false;
        }
        uint32_t crop_x = 0;
        uint32_t crop_y = 0;
        uint32_t crop_w = 0;
        uint32_t crop_h = 0;
        cover_rect(width, height, dst_w, dst_h,
                   &crop_x, &crop_y, &crop_w, &crop_h);
        align_even_rect(width, height, &crop_x, &crop_y, &crop_w, &crop_h);
        if(crop_w < 2u || crop_h < 2u) {
            return false;
        }

        const size_t dst_bytes = static_cast<size_t>(dst_w) * dst_h * 2u;
        upc_ipc::DmaHeapBuffer dst_dma{};
        if(!upc_ipc::dma_heap_alloc(dst_bytes, dst_dma,
                                    upc_ipc::DmaHeapUsage::RgaDevice)) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail rga dst dma_heap alloc failed bytes=%zu",
                dst_bytes);
            return false;
        }
        rga_buffer_handle_t dst_hnd =
            importbuffer_fd(dst_dma.fd, static_cast<int>(dst_bytes));
        if(dst_hnd == 0) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail dest importbuffer_fd failed bytes=%zu heap=%s",
                dst_bytes, dst_dma.heap_path ? dst_dma.heap_path : "?");
            upc_ipc::dma_heap_free(dst_dma);
            return false;
        }

        const im_rect crop{
            static_cast<int>(crop_x), static_cast<int>(crop_y),
            static_cast<int>(crop_w), static_cast<int>(crop_h)};
        const bool two_pass =
            crop_w > dst_w * kRgaMaxScale || crop_h > dst_h * kRgaMaxScale;
        bool ok = false;
        if(!two_pass) {
            ok = rga_process(handle,
                             static_cast<int>(width), static_cast<int>(height),
                             kRgaSrcFormat, crop,
                             dst_hnd,
                             static_cast<int>(dst_w), static_cast<int>(dst_h),
                             kRgaDstFormat);
        } else {
            uint32_t mid_w = (crop_w + kRgaMaxScale - 1u) / kRgaMaxScale;
            uint32_t mid_h = (crop_h + kRgaMaxScale - 1u) / kRgaMaxScale;
            mid_w = std::max(dst_w, (mid_w + 1u) & ~1u);
            mid_h = std::max(dst_h, (mid_h + 1u) & ~1u);
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
                                     static_cast<int>(dst_w),
                                     static_cast<int>(dst_h),
                                     kRgaDstFormat);
                    releasebuffer_handle(mid_hnd);
                }
                upc_ipc::dma_heap_free(mid_dma);
            }
        }

        if(ok) {
            dma_buf_sync(dst_dma.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            rgb565->assign(static_cast<const uint8_t*>(dst_dma.va),
                           static_cast<const uint8_t*>(dst_dma.va) + dst_bytes);
            dma_buf_sync(dst_dma.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        }
        releasebuffer_handle(dst_hnd);
        upc_ipc::dma_heap_free(dst_dma);
        return ok;
    }
};
#endif

void render_cpu(const DecodedPng& src, uint32_t dst_w, uint32_t dst_h,
                std::vector<uint8_t>* rgb565)
{
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = 0;
    uint32_t crop_h = 0;
    cover_rect(src.width, src.height, dst_w, dst_h,
               &crop_x, &crop_y, &crop_w, &crop_h);

    rgb565->assign(static_cast<size_t>(dst_w) * dst_h * 2u, 0);
    for(uint32_t y = 0; y < dst_h; ++y) {
        const uint64_t fy = ((static_cast<uint64_t>(y) * 2u + 1u) * crop_h << 15) /
                            dst_h;
        const uint64_t sy_fp = (static_cast<uint64_t>(crop_y) << 16) +
                               (fy > 32768u ? fy - 32768u : 0u);
        const uint32_t sy0 = std::min<uint32_t>(src.height - 1u, sy_fp >> 16);
        const uint32_t sy1 = std::min<uint32_t>(src.height - 1u, sy0 + 1u);
        const uint32_t wy = static_cast<uint32_t>(sy_fp & 0xffffu);
        for(uint32_t x = 0; x < dst_w; ++x) {
            const uint64_t fx = ((static_cast<uint64_t>(x) * 2u + 1u) * crop_w << 15) /
                                dst_w;
            const uint64_t sx_fp = (static_cast<uint64_t>(crop_x) << 16) +
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
            const uint8_t r = channel(0);
            const uint8_t g = channel(1);
            const uint8_t b = channel(2);
            const uint16_t pixel = static_cast<uint16_t>(
                ((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
            const size_t offset = (static_cast<size_t>(y) * dst_w + x) * 2u;
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
    uint32_t decode_edge = 0;
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
            rendered = rga_src.render(target.width, target.height, &pixels);
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
            render_cpu(decoded, target.width, target.height, &pixels);
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

#include "transfer_thumbnail_generator.hpp"

#include "raw_thumb_format.hpp"
#include "rs_log.hpp"

#include <png.h>

#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
#include <rga/im2d.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
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

bool decode_png_rgba(const std::string& path, DecodedPng* decoded, std::string* error)
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

    decoded->rgba.assign(static_cast<size_t>(width) * height * 4u, 0);
    for(uint32_t y = 0; y < height; ++y) {
        png_bytep row =
            decoded->rgba.data() + static_cast<size_t>(y) * width * 4u;
        png_read_row(png, row, nullptr);
    }
    png_read_end(png, nullptr);
    decoded->width = width;
    decoded->height = height;
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

#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
bool rga_status_ok(IM_STATUS status)
{
    return status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
}

bool render_rga(const DecodedPng& src, uint32_t dst_w, uint32_t dst_h,
                std::vector<uint8_t>* rgb565)
{
    if(src.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
       src.height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
       dst_w > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
       dst_h > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = 0;
    uint32_t crop_h = 0;
    cover_rect(src.width, src.height, dst_w, dst_h,
               &crop_x, &crop_y, &crop_w, &crop_h);
    rgb565->assign(static_cast<size_t>(dst_w) * dst_h * 2u, 0);

    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(src.rgba.data()),
        static_cast<int>(src.width), static_cast<int>(src.height),
        RK_FORMAT_RGBA_8888);
    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(
        rgb565->data(), static_cast<int>(dst_w), static_cast<int>(dst_h),
        RK_FORMAT_RGB_565);
    rga_buffer_t pat{};
    const im_rect src_rect{
        static_cast<int>(crop_x), static_cast<int>(crop_y),
        static_cast<int>(crop_w), static_cast<int>(crop_h)};
    const im_rect dst_rect{0, 0, static_cast<int>(dst_w), static_cast<int>(dst_h)};
    const im_rect pat_rect{};
    const IM_STATUS checked = imcheck(src_buf, dst_buf, src_rect, dst_rect);
    if(!rga_status_ok(checked)) {
        return false;
    }
    return rga_status_ok(
        improcess(src_buf, dst_buf, pat, src_rect, dst_rect, pat_rect, IM_SYNC));
}
#endif

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
    DecodedPng decoded;
    if(!decode_png_rgba(png_path, &decoded, error)) {
        return false;
    }

    std::vector<std::string> created;
    for(const TransferThumbnailTarget& target : targets) {
        std::vector<uint8_t> pixels;
        bool rendered = false;
#if defined(UPC_XFR_HAS_RGA) && UPC_XFR_HAS_RGA
        rendered = render_rga(decoded, target.width, target.height, &pixels);
        if(!rendered) {
            UPC_LOG_XFR_SERVICE(Warn,
                "3mf thumbnail RGA failed, fallback CPU png=%s dst=%ux%u",
                png_path.c_str(), static_cast<unsigned>(target.width),
                static_cast<unsigned>(target.height));
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

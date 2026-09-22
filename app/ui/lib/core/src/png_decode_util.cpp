#include "png_decode_util.hpp"

#include <cstdio>

#if defined(UPC_UI_HAS_LIBPNG) && UPC_UI_HAS_LIBPNG
#include <png.h>
#endif

namespace ui::png_util {

bool decode_rgba(const char* path, std::vector<uint8_t>& rgba,
                 uint32_t& w, uint32_t& h)
{
#if defined(UPC_UI_HAS_LIBPNG) && UPC_UI_HAS_LIBPNG
    if(!path || !path[0]) {
        return false;
    }
    FILE* fp = std::fopen(path, "rb");
    if(!fp) {
        return false;
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    if(!png) {
        std::fclose(fp);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if(!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return false;
    }
    if(setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    w = png_get_image_width(png, info);
    h = png_get_image_height(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);
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
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY ||
       color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_read_update_info(png, info);
    if(w == 0 || h == 0 || w > 8192 || h > 8192) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return false;
    }
    rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
    std::vector<png_bytep> rows(h);
    for(uint32_t y = 0; y < h; ++y) {
        rows[y] = rgba.data() + static_cast<size_t>(y) * w * 4u;
    }
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return true;
#else
    (void)path;
    (void)rgba;
    (void)w;
    (void)h;
    return false;
#endif
}

void scale_to_rgb565_point(const uint8_t* rgba, uint32_t src_w, uint32_t src_h,
                           uint8_t* out, int32_t dst_w, int32_t dst_h)
{
    if(!rgba || !out || src_w == 0 || src_h == 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    for(int32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy = static_cast<uint32_t>(y) * src_h /
                           static_cast<uint32_t>(dst_h);
        for(int32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx = static_cast<uint32_t>(x) * src_w /
                               static_cast<uint32_t>(dst_w);
            const uint8_t* p =
                rgba + (static_cast<size_t>(sy) * src_w + sx) * 4u;
            const uint16_t r = p[0] >> 3;
            const uint16_t g = p[1] >> 2;
            const uint16_t b = p[2] >> 3;
            const uint16_t pix =
                static_cast<uint16_t>((r << 11) | (g << 5) | b);
            const size_t off =
                (static_cast<size_t>(y) * dst_w + x) * 2u;
            out[off] = static_cast<uint8_t>(pix & 0xFFu);
            out[off + 1] = static_cast<uint8_t>((pix >> 8) & 0xFFu);
        }
    }
}

void scale_to_rgb565_box(const uint8_t* rgba, uint32_t src_w, uint32_t src_h,
                         uint8_t* out, int32_t dst_w, int32_t dst_h)
{
    if(!rgba || !out || src_w == 0 || src_h == 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    for(int32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy0 = static_cast<uint32_t>(y) * src_h /
                             static_cast<uint32_t>(dst_h);
        uint32_t sy1 = static_cast<uint32_t>(y + 1) * src_h /
                       static_cast<uint32_t>(dst_h);
        /* 放大（或 1:1）时上下界会算成同一行，至少取一行，否则整行采不到样本 */
        if(sy1 <= sy0) {
            sy1 = sy0 + 1u;
        }
        if(sy1 > src_h) {
            sy1 = src_h;
        }
        for(int32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx0 = static_cast<uint32_t>(x) * src_w /
                                 static_cast<uint32_t>(dst_w);
            uint32_t sx1 = static_cast<uint32_t>(x + 1) * src_w /
                           static_cast<uint32_t>(dst_w);
            if(sx1 <= sx0) {
                sx1 = sx0 + 1u;
            }
            if(sx1 > src_w) {
                sx1 = src_w;
            }

            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t n = 0;
            for(uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint8_t* row =
                    rgba + (static_cast<size_t>(sy) * src_w + sx0) * 4u;
                for(uint32_t sx = sx0; sx < sx1; ++sx, row += 4) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    ++n;
                }
            }
            if(n == 0u) {
                n = 1u;
            }
            const uint16_t pix = static_cast<uint16_t>(
                (((r / n) >> 3) << 11) | (((g / n) >> 2) << 5) | ((b / n) >> 3));
            const size_t off = (static_cast<size_t>(y) * dst_w + x) * 2u;
            out[off] = static_cast<uint8_t>(pix & 0xFFu);
            out[off + 1] = static_cast<uint8_t>((pix >> 8) & 0xFFu);
        }
    }
}

void contain_fit(uint32_t src_w, uint32_t src_h, int32_t box_w, int32_t box_h,
                 int32_t& out_w, int32_t& out_h)
{
    out_w = 1;
    out_h = 1;
    if(src_w == 0 || src_h == 0 || box_w <= 0 || box_h <= 0) {
        return;
    }
    if(static_cast<int32_t>(src_w) <= box_w &&
       static_cast<int32_t>(src_h) <= box_h) {
        out_w = static_cast<int32_t>(src_w);
        out_h = static_cast<int32_t>(src_h);
        return;
    }
    /* 定点比较，避开浮点：贴宽后的高 = by_w/src_w，贴高后的宽 = by_h/src_h。
     * 「贴宽」可行的条件是贴宽后的高不超过 box_h，整理后就是 by_w <= by_h。 */
    const int64_t by_w = static_cast<int64_t>(box_w) * src_h;
    const int64_t by_h = static_cast<int64_t>(box_h) * src_w;
    if(by_w <= by_h) {
        out_w = box_w;
        out_h = static_cast<int32_t>(by_w / src_w);
    } else {
        out_h = box_h;
        out_w = static_cast<int32_t>(by_h / src_h);
    }
    if(out_w < 1) {
        out_w = 1;
    }
    if(out_h < 1) {
        out_h = 1;
    }
}

} // namespace ui::png_util

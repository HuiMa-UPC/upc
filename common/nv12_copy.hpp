#ifndef UPC_NV12_COPY_HPP
#define UPC_NV12_COPY_HPP

#include <cstdint>
#include <cstring>

namespace upc {

/**
 * 把带 stride 的 NV12 平面拷成紧凑缓冲（Y 后接 UV，行距 = width）。
 * stride == width 时整块 memcpy；否则逐行丢掉行尾 padding。
 */
inline void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                            const uint8_t* src_uv, uint32_t src_stride_y,
                            uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || src_stride_y == 0 || width == 0 ||
       height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }
    for(uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * width,
                    src_y + static_cast<size_t>(row) * src_stride_y, width);
    }
    for(uint32_t row = 0; row < height / 2u; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * width,
                    src_uv + static_cast<size_t>(row) * src_stride_y, width);
    }
}

} // namespace upc

#endif

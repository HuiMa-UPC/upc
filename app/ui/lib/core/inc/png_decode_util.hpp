#ifndef UI_PNG_DECODE_UTIL_HPP
#define UI_PNG_DECODE_UTIL_HPP

#include <cstdint>
#include <vector>

namespace ui::png_util {

/**
 * 静图解码与缩放的公共实现，由拖动预览（png_scrub_preview）与相册大图
 * （photo_decoder）共用。
 *
 * JPEG（.jpg / .jpeg）走 libturbojpeg，是相机拍照与延时层拍的格式。
 * PNG 仍留给非相机来源。全部函数都可在后台线程调用：不碰 LVGL。
 */

/**
 * 静图 → RGBA8888。`.jpg`/`.jpeg` 用 turbojpeg；其余当 PNG（无 libpng 时失败）。
 *
 * 宽高超过 8192 视为异常输入直接拒绝。
 */
bool decode_rgba(const char* path, std::vector<uint8_t>& rgba,
                 uint32_t& w, uint32_t& h);

/**
 * RGBA → RGB565，最近邻点采样。
 * 快，但大幅降采样会有锯齿/闪点，用于拖动预览那种一闪而过的小图。
 */
void scale_to_rgb565_point(const uint8_t* rgba, uint32_t src_w, uint32_t src_h,
                           uint8_t* out, int32_t dst_w, int32_t dst_h);

/**
 * RGBA → RGB565，源矩形均值（box filter）。
 * 比点采样慢一个常数倍，但降采样干净，用于一次解出来要长期显示的大图。
 */
void scale_to_rgb565_box(const uint8_t* rgba, uint32_t src_w, uint32_t src_h,
                         uint8_t* out, int32_t dst_w, int32_t dst_h);

/**
 * 等比贴合（CONTAIN）：算出把 src 放进 box 的最大尺寸。
 *
 * **不放大**：源比 box 小时原样返回源尺寸。放大交给 LVGL 的
 * LV_IMAGE_ALIGN_CONTAIN 在绘制期做，解码缓冲不必为此变大。
 */
void contain_fit(uint32_t src_w, uint32_t src_h, int32_t box_w, int32_t box_h,
                 int32_t& out_w, int32_t& out_h);

} // namespace ui::png_util

#endif /* UI_PNG_DECODE_UTIL_HPP */

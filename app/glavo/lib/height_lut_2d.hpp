#ifndef HEIGHT_LUT_2D_HPP
#define HEIGHT_LUT_2D_HPP

/**
 * @file height_lut_2d.hpp
 * @brief 多点振镜校准的二维高度查表（Glavo 侧专用）。
 *
 * 背景：x=0 校准时激光与相机光轴共面，基板反射散光无法测量，故测高需 x≠0。
 * 但 x≠0 时高度变化会引起图像 img_x 与 img_y 同时移动，单维 img_y 查表不准。
 *
 * 本库按 (galvo_x, galvo_y) 分组回归（当前 8 点：x=±30/±35，y=0/10/20）：
 *   - LUT 文件每行 5 列：height  galvo_x  galvo_y  img_x  img_y
 *   - 加载时按 (galvo_x, galvo_y) 分组，每组做最小二乘线性回归：
 *       img_x = a0 + a1·h
 *       img_y = b0 + b1·h
 *   - 查表用二维最小二乘投影（融合 img_x/img_y 两路信息）：
 *       h = (a1·(img_x-a0) + b1·(img_y-b0)) / (a1² + b1²)
 *     a1=0 时自动退化为只用 img_y，兼容 x=0 单维情形。
 *
 * 测量时在各振镜位置各测一次 (img_x, img_y)，用对应系数查 h，再取中位数。
 */

#include <cstddef>
#include <string>
#include <vector>

namespace upc::glavo {

/** 单个振镜位置的回归系数（由该位置的所有校准样本回归得到）。 */
struct GalvoLutEntry {
    float galvo_x;   ///< 振镜 X 位置(mm)
    float galvo_y;   ///< 振镜 Y 位置(mm)
    float a0, a1;    ///< img_x = a0 + a1·h
    float b0, b1;    ///< img_y = b0 + b1·h
    int   count;     ///< 该位置样本数（调试用）
};

/** 二维高度查表（按振镜位置分组）。 */
class HeightLut2d {
public:
    /** 加载 5 列 LUT 文件：height galvo_x galvo_y img_x img_y。
     *  按 (galvo_x, galvo_y) 分组回归。返回是否成功（至少 1 个位置有 >=2 样本）。 */
    bool load(const std::string& path);

    /** 只加载高度落在 [h_min, h_max] 内的样本（精测用 0 附近加密区）。 */
    bool load(const std::string& path, float h_min, float h_max);

    /** 二维查表：在 (galvo_x, galvo_y) 处测得 (img_x, img_y)，反求高度(mm)。
     *  内部找最近的校准位置，用其 (a0,a1,b0,b1) 做最小二乘投影。
     *  越界或无数据返回 NaN。 */
    float lookup(float galvo_x, float galvo_y,
                 float img_x, float img_y) const;

    bool valid() const { return !entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    const std::vector<GalvoLutEntry>& entries() const { return entries_; }

private:
    std::vector<GalvoLutEntry> entries_;
};

} // namespace upc::glavo

#endif // HEIGHT_LUT_2D_HPP

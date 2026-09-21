#ifndef HEIGHT_LUT_2D_HPP
#define HEIGHT_LUT_2D_HPP

/**
 * @file height_lut_2d.hpp
 * @brief 按振镜位置分组的二维高度 LUT（Glavo）。
 *
 * 5 列：height galvo_x galvo_y img_x img_y。
 * 每组回归 img_x=a0+a1·h、img_y=b0+b1·h，
 * 反查 h=(a1·(img_x-a0)+b1·(img_y-b0))/(a1²+b1²)。
 */

#include <cstddef>
#include <string>
#include <vector>

namespace upc::glavo {

struct GalvoLutEntry {
    float galvo_x;
    float galvo_y;
    float a0, a1;  /**< img_x = a0 + a1·h */
    float b0, b1;  /**< img_y = b0 + b1·h */
    int   count;
};

class HeightLut2d {
public:
    /** 至少 1 组、每组 ≥2 样本才算成功。 */
    bool load(const std::string& path);
    /** 只加载 [h_min, h_max] 内样本（精测 ±1 mm）。 */
    bool load(const std::string& path, float h_min, float h_max);
    /** 最近振镜组投影；无数据返回 NaN。 */
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

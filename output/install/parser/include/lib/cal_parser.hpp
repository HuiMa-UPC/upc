#ifndef CAL_PARSER_HPP
#define CAL_PARSER_HPP

#include "parser_config.hpp"

#include <cstddef>
#include <vector>

namespace upc::parser {

struct CalData {
    bool valid = false;
    int nx = 0;
    int ny = 0;
    double grid_x = 0.0;
    double grid_y = 0.0;
    double x_min = 0.0;
    double x_max = 0.0;
    double y_min = 0.0;
    double y_max = 0.0;
    std::vector<double> z_dx; /**< row-major: z_dx[i + j*nx] */
    std::vector<double> z_dy;

    bool has_gains = false;
    float scale_gain = 0.f;   /**< 图像大小增益 */
    float range_gain = 0.f;   /**< 图像范围 */
    float speed_gain = 0.f;   /**< 速度增益 */

    double center_offset_x = 0.0; /**< CSV 第 4 行 X,<val> 中心平移（mm） */
    double center_offset_y = 0.0; /**< CSV 第 4 行 Y,<val> 中心平移（mm） */
};

/** 清空校准数据 */
void cal_data_clear(CalData *cal);

/** 解析 CSV 校准文件（格式见 doc/cal.csv）
 *  @return 0 成功，-1 失败
 */
int cal_parse_csv(const char *path, CalData *out);

/** 在 (x,y) 双线性插值 ΔX/ΔY
 *  @return 1 成功，0 失败（cal 无效或越界）
 */
int cal_interpolate_delta(const CalData *cal, double x, double y,
                          double *out_dx, double *out_dy);

/** 切片坐标 → 校准后物理坐标：中心平移 + 网格双线性 Δ（查表用原始切片坐标） */
void cal_apply_xy(const CalData *cal, float slice_x, float slice_y,
                  float *out_x, float *out_y);

} // namespace upc::parser

#endif // CAL_PARSER_HPP

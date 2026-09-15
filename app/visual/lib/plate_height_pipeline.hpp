/**
 * 激光光斑查表测高 — 公共 API (C++ / OpenCV)
 *
 * 本库为自包含算法库 (对标 frame_diff_pipeline)：只接受 cv::Mat + 配置路径，
 * 不依赖 IPC / 取流 / NV12，便于算法同学独立调试与替换。
 *
 * 算法流程 (移植自 LaserSpotDetection):
 *   1. (可选) 预处理灰度图        —— IPreProcessor 策略，可插拔
 *   2. 光斑分割得中心 (x, y)       —— ISpotSegmenter 策略，可替换
 *   3. 用 laser_y 查 LUT 线性插值    —— HeightLut
 *   4. 输出高度 (mm)
 *
 * 配置全部从 plate_height_config.json 读取 (ROI / 阈值 / LUT 路径等)，
 * 调参只改 JSON 不重编译。
 *
 * 扩展方式:
 *   - 换分割算法: 实现 ISpotSegmenter，调带策略的重载 measure_height()
 *   - 加预处理:   实现 IPreProcessor，调带策略的重载 measure_height()
 *   均无需改动本库源码。
 */
#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace upc::visual {

// -------------------- 数据结构 --------------------

/** 光斑分割结果。mask 仅供调试，可为空。 */
struct LaserSpot {
    bool valid{false};
    float center_x{0.0f};
    float center_y{0.0f};
    cv::Mat mask;  // CV_8U, 255=光斑区域；调试用，可空
};

/** 单条标定样本：高度(mm) 与对应光斑像素中心。 */
struct HeightSample {
    float height{0.0f};   ///< mm
    float laser_x{0.0f};
    float laser_y{0.0f};
};

// -------------------- 配置 (JSON 驱动) --------------------

struct PlateHeightConfig {
    // —— 分割 ——
    std::vector<cv::Point> roi_quad; ///< 光斑搜索区(不规则四边形，4 个顶点)；空表示整幅图
    uint8_t fixed_thresh{250};   ///< 二值化阈值
    int min_area{5};             ///< 光斑最小像素面积
    int max_area{200};            ///< 光斑最大像素面积

    // —— LUT ——
    std::string lut_path;         ///< 标定表路径 (height_lut.txt)

    // —— 预处理 (预留，默认全关) ——
    bool enable_clahe{false};
    double clahe_clip{3.0};
    bool enable_blur{false};
    int blur_k{3};
};

/** 从 plate_height_config.json 解析配置；缺失字段用默认值。 */
PlateHeightConfig load_plate_height_config(const std::string& config_path);

// -------------------- 策略接口 (可替换 / 可扩展) --------------------

/** 预处理策略：输入灰度图，输出处理后的灰度图。 */
class IPreProcessor {
public:
    virtual ~IPreProcessor() = default;
    virtual cv::Mat process(const cv::Mat& gray,
                             const PlateHeightConfig& cfg) const = 0;
};

/** 光斑分割策略：输入 (预处理后的) 灰度图，输出光斑中心。 */
class ISpotSegmenter {
public:
    virtual ~ISpotSegmenter() = default;
    virtual LaserSpot segment(const cv::Mat& gray,
                              const PlateHeightConfig& cfg) const = 0;
};

// -------------------- 默认实现 (来自源算法) --------------------

/** 直通预处理：原样返回，不做任何处理。 */
class NullPreProcessor : public IPreProcessor {
public:
    cv::Mat process(const cv::Mat& gray, const PlateHeightConfig& cfg) const override;
};

/** CLAHE 对比度增强预处理 (预留，默认不开)。 */
class ClahePreProcessor : public IPreProcessor {
public:
    cv::Mat process(const cv::Mat& gray, const PlateHeightConfig& cfg) const override;
};

/**
 * 固定阈值分割 (移植自 seg::fixedThresholdSegment)：
 *   ROI 裁剪 → 阈值二值化 → 连通域 → 取面积最大且在 [min,max] 区间的块
 *   → 灰度加权质心求光斑中心 (坐标在原图坐标系)。
 */
class FixedThresholdSegmenter : public ISpotSegmenter {
public:
    LaserSpot segment(const cv::Mat& gray, const PlateHeightConfig& cfg) const override;
};

// -------------------- LUT 管理 --------------------

/**
 * 高度标定表：加载 + 查表插值。
 * 文本格式 (每行):  height  laser_x  laser_y
 * 内部按 height 升序排序，用 laser_y 做线性插值。
 */
class HeightLut {
public:
    /** 加载标定表；返回是否成功。失败时 valid() 为 false。 */
    bool load(const std::string& path);

    /**
     * 保存标定表到文本文件（3列：height laser_x laser_y）。
     * 内部按 height 升序排序后写入，保证与 load() 格式一致。
     * @param path    输出文件路径
     * @param samples 标定样本（顺序任意，内部会排序）
     * @return 是否写入成功
     */
    static bool save(const std::string& path,
                     const std::vector<HeightSample>& samples);

    /** 用 laser_y 查表线性插值求高度(mm)；越界或表空返回 NaN。 */
    float lookup(float laser_y) const;

    bool valid() const { return !tables_.empty() && zero_index_ >= 0; }
    std::size_t size() const { return tables_.size(); }

private:
    std::vector<HeightSample> tables_;
    int zero_index_{-1};      ///< height≈0 的索引
    bool y_descending_{true}; ///< height 升序时 laser_y 是否递减
};

// -------------------- 顶层入口 (对标 detect_powder_defect) --------------------

/**
 * 测高 (默认策略)：内部加载配置 + 默认 NullPreProcessor + FixedThresholdSegmenter。
 * @param gray      输入灰度图 (CV_8UC1)
 * @param config_path  plate_height_config.json 路径
 * @return 高度(mm)；失败返回 NaN。
 */
float measure_height(const cv::Mat& gray, const std::string& config_path);

/**
 * 只做光斑分割，返回质心像素坐标（校准 / 打印前测高用）。
 * Visual 不查 LUT；高度由 Glavo 侧 HeightLut2d 计算。
 */
LaserSpot detect_laser_spot(const cv::Mat& gray, const std::string& config_path);

/**
 * 测高 (自定义策略)：传入已加载的配置与策略实例。
 * 想换分割器 / 加预处理时用此重载，无需改库源码。
 * @return 高度(mm)；失败返回 NaN。
 */
float measure_height(const cv::Mat& gray,
                     const PlateHeightConfig& cfg,
                     const IPreProcessor& pre,
                     const ISpotSegmenter& seg);

} // namespace upc::visual

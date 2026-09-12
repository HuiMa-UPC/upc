/**
 * @file red_dot_detector.hpp
 * @brief 打印前光路检测的红点识别接口（算法待视觉侧实现）。
 *
 * =============================================================================
 * 这是给视觉工程师的接入点
 * =============================================================================
 * 当前 red_dot_detector.cpp 只是一个 fail-closed 的桩：恒返回 found=false。
 * 视觉侧只需实现 RedDotDetector::Impl，不必改动 VisualService / Glavo / UI。
 * 完整背景、调参建议与联调步骤见 doc/视觉协作-光路检测.md。
 *
 * 输入帧格式：紧凑（无 stride padding）NV12
 *   - Y  平面：nv12[0 .. width*height)              灰度
 *   - UV 平面：nv12[width*height .. +width*height/2) U/V 交错，半分辨率
 *   - bytes 恒为 width*height*3/2
 *   红点判定必须用色度，所以这里传的是整帧 NV12（首层检测那条链路只用 Y 平面）。
 *
 * 调用时序：VisualService 的取点线程在判定窗口内按帧轮询，每来一帧新图调一次
 * detect_nv12()，一旦 found=true 立即回包给 Glavo 并停止本点轮询。
 *
 * 线程约定：同一时刻只有取点线程调用本类（Glavo 严格逐点串行，一点一回包），
 * 但该线程与 VisualService 的取帧线程是两条线程，Impl 内部不要持有跨调用的
 * 非常量全局状态；需要缓存就自己加锁。
 */
#ifndef UPC_RED_DOT_DETECTOR_HPP
#define UPC_RED_DOT_DETECTOR_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace upc::visual {

/** 本点的先验信息：由 Glavo 按振镜坐标给出，可用于收窄 ROI。 */
struct RedDotHint {
    /** 期望振镜坐标（mm）。当前五点为 (0,0) 与 (±30,±30)。 */
    double expect_x_mm = 0.0;
    double expect_y_mm = 0.0;
    /** 点位序号 0=中心 1=右上 2=右下 3=左下 4=左上。 */
    uint8_t point_index = 0;
    /** 本点总数（当前 5），便于算法按序号推断相对方位。 */
    uint8_t point_total = 5;
};

/** 单帧识别结果。 */
struct RedDotDetection {
    /** true=本帧确认看到红点。只有它决定放行，其余字段仅进日志。 */
    bool found = false;
    /** 红点像素中心（左上原点）；found=false 时无意义。 */
    double center_x = 0.0;
    double center_y = 0.0;
    /** 红点连通域面积（像素）；用于日志里判断是否把噪点当成了红点。 */
    double area_px = 0.0;
    /** 置信度 0.0~1.0，跨进程按 ×256 传给 Glavo，仅用于日志。 */
    double confidence = 0.0;
    /** 本帧耗时（ms），用于确认轮询周期是否够快。 */
    double total_ms = 0.0;
    /** found=false 时说明为什么（如 no_red_pixels / area_too_small）。 */
    std::string reason = "not_implemented";
    /** 非空表示算法自身出错（模型缺失等）；一律按 found=false 处理。 */
    std::string error;
};

/**
 * @brief 红光点识别器
 *
 * 实现要求（务必遵守，否则会把光路故障漏放过去）：
 *   1. fail-closed：任何异常、参数非法、内部报错都返回 found=false，绝不返回 true。
 *   2. 无状态判定：单帧能确认就返回 true，不要跨帧累计"大概是"。
 *   3. 快：单帧应远小于轮询周期（目标 <30ms），否则 3 秒窗口内取不到足够帧。
 */
class RedDotDetector {
public:
    RedDotDetector();
    ~RedDotDetector();

    RedDotDetector(const RedDotDetector&) = delete;
    RedDotDetector& operator=(const RedDotDetector&) = delete;

    /**
     * @brief 在一帧紧凑 NV12 上识别红点
     * @param nv12      紧凑 NV12 首地址（Y 平面在前，UV 交错在后）
     * @param bytes     缓冲长度，应等于 width*height*3/2
     * @param width     帧宽（像素）
     * @param height    帧高（像素）
     * @param frame_seq 相机帧序号，仅用于日志对齐
     * @param hint      本点先验（期望振镜坐标与点位序号）
     */
    RedDotDetection detect_nv12(const uint8_t* nv12, std::size_t bytes,
                                uint32_t width, uint32_t height,
                                uint32_t frame_seq, const RedDotHint& hint);

    /** @brief 一轮光路检测开始时调用，用于清算法侧的跨点缓存。 */
    void reset_session();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace upc::visual

#endif

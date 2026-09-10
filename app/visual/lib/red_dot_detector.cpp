/**
 * @file red_dot_detector.cpp
 * @brief 红点识别桩实现：算法留给视觉侧填，当前一律 fail-closed 返回未识别。
 *
 * 接入方式（视觉工程师看这里）：
 *   本文件里的 Impl::detect 就是全部要写的地方。上下游（取流、判定窗口轮询、
 *   IPC 回包、UI 徽标、失败阻断与报警）都已经通了，无需改其它文件。
 *   把 kStubEnabled 相关的桩逻辑替换成真实算法即可。
 *
 * 桩当前行为：
 *   - 恒返回 found=false，reason="not_implemented"。
 *   - 因此把调试页「Optical Path Chk」打开后，第 0 点就会在 3 秒后判失败。
 *     这正是设计意图（fail-closed），也方便先单独验证阻断链路是否完整。
 *   - 算法未接入前请保持该开关关闭（默认关）。
 *
 * 实现建议见 doc/视觉协作-光路检测.md，要点摘录：
 *   - NV12 的 UV 平面在 nv12 + width*height，U/V 交错、半分辨率。
 *   - 红点在 YUV 里的特征是 V 显著高于 128 且 Y 偏高；先做色度阈值再取连通域，
 *     比转 BGR 再走 HSV 快得多。
 *   - 相机自动曝光会被亮红点拉低整体亮度，不要用全局亮度做前置判据。
 */
#include "red_dot_detector.hpp"

#include "rs_log.hpp"

namespace upc::visual {

class RedDotDetector::Impl {
public:
    RedDotDetection detect(const uint8_t* nv12, std::size_t bytes,
                           uint32_t width, uint32_t height,
                           uint32_t frame_seq, const RedDotHint& hint)
    {
        RedDotDetection out;
        out.found = false;

        /* 入参自检：桩阶段也把它保留下来，视觉侧实现后同样需要这段前置校验。 */
        const std::size_t expect = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 3u / 2u;
        if(nv12 == nullptr || width == 0u || height == 0u || bytes < expect) {
            out.reason = "invalid_frame";
            out.error = "frame geometry mismatch";
            UPC_LOG_VISUAL_SERVICE(Warn,
                "red dot detect invalid frame seq=%u width=%u height=%u bytes=%zu",
                static_cast<unsigned>(frame_seq),
                static_cast<unsigned>(width), static_cast<unsigned>(height),
                bytes);
            return out;
        }

        /* ================= 视觉算法接入点：以下 return 替换为真实实现 ================= */
        out.reason = "not_implemented";
        UPC_LOG_VISUAL_SERVICE(Debug,
            "red dot detect stub seq=%u point=%u expect_mm=%.1f,%.1f result=0",
            static_cast<unsigned>(frame_seq),
            static_cast<unsigned>(hint.point_index),
            hint.expect_x_mm, hint.expect_y_mm);
        return out;
        /* ============================================================================ */
    }

    void reset() {}
};

RedDotDetector::RedDotDetector() : impl_(std::make_unique<Impl>()) {}
RedDotDetector::~RedDotDetector() = default;

RedDotDetection RedDotDetector::detect_nv12(const uint8_t* nv12, std::size_t bytes,
                                            uint32_t width, uint32_t height,
                                            uint32_t frame_seq,
                                            const RedDotHint& hint)
{
    return impl_->detect(nv12, bytes, width, height, frame_seq, hint);
}

void RedDotDetector::reset_session()
{
    impl_->reset();
}

} // namespace upc::visual

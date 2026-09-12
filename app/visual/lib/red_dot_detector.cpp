/**
 * @file red_dot_detector.cpp
 * @brief 基于 NV12 色度与连通域的单帧红点识别。
 *
 * 每个 UV 样本对应原图 2x2 像素。先用 V、V-U 和局部 Y 峰值生成半分辨率
 * 掩码，再做 8 邻域连通域；不做 NV12->BGR/HSV 转换，也不依赖跨帧状态。
 */
#include "red_dot_detector.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <vector>

namespace upc::visual {

namespace {

/* 阈值由同机位实机五点录像起调。原始 NV12 不经过视频压缩，通常会比录像中的
 * 色度更集中；这里仍为最弱的左上点保留少量余量。 */
constexpr uint8_t kMinV = 150;
constexpr int kMinVMinusU = 35;
constexpr uint8_t kMinLocalY = 45;

/* 半分辨率上至少两个相邻色度样本，即原图约 8px；拒绝单个 UV 热点。 */
constexpr uint32_t kMinComponentCells = 2;

double elapsed_ms(const std::chrono::steady_clock::time_point& started)
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

double clamp01(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

} // namespace

class RedDotDetector::Impl {
public:
    RedDotDetection detect(const uint8_t* nv12, std::size_t bytes,
                           uint32_t width, uint32_t height,
                           uint32_t frame_seq, const RedDotHint& hint)
    {
        const auto started = std::chrono::steady_clock::now();
        RedDotDetection out;
        out.found = false;

        const auto finish = [&started](RedDotDetection& result) {
            result.total_ms = elapsed_ms(started);
            return result;
        };

        /* NV12 要求偶数宽高。先查乘法溢出，再验证紧凑缓冲的精确长度。 */
        if(nv12 == nullptr || width < 2u || height < 2u ||
           (width & 1u) != 0u || (height & 1u) != 0u ||
           static_cast<std::size_t>(width) >
               std::numeric_limits<std::size_t>::max() /
                   static_cast<std::size_t>(height)) {
            out.reason = "invalid_frame";
            out.error = "frame geometry mismatch";
            finish(out);
            UPC_LOG_VISUAL_SERVICE(Warn,
                "red dot detect invalid frame seq=%u width=%u height=%u bytes=%zu",
                static_cast<unsigned>(frame_seq),
                static_cast<unsigned>(width), static_cast<unsigned>(height),
                bytes);
            return out;
        }

        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        if(pixels > std::numeric_limits<std::size_t>::max() - pixels / 2u) {
            out.reason = "invalid_frame";
            out.error = "frame byte count overflow";
            finish(out);
            UPC_LOG_VISUAL_SERVICE(Warn,
                "red dot detect frame size overflow seq=%u width=%u height=%u",
                static_cast<unsigned>(frame_seq),
                static_cast<unsigned>(width), static_cast<unsigned>(height));
            return out;
        }
        const std::size_t expected_bytes = pixels + pixels / 2u;
        if(bytes != expected_bytes) {
            out.reason = "invalid_frame";
            out.error = "frame byte count mismatch";
            finish(out);
            UPC_LOG_VISUAL_SERVICE(Warn,
                "red dot detect invalid bytes seq=%u width=%u height=%u bytes=%zu expected=%zu",
                static_cast<unsigned>(frame_seq),
                static_cast<unsigned>(width), static_cast<unsigned>(height),
                bytes, expected_bytes);
            return out;
        }
        if(hint.point_total == 0u || hint.point_index >= hint.point_total) {
            out.reason = "invalid_hint";
            out.error = "point index out of range";
            finish(out);
            UPC_LOG_VISUAL_SERVICE(Warn,
                "red dot detect invalid hint seq=%u point=%u total=%u",
                static_cast<unsigned>(frame_seq),
                static_cast<unsigned>(hint.point_index),
                static_cast<unsigned>(hint.point_total));
            return out;
        }

        try {
            const uint32_t chroma_width = width / 2u;
            const uint32_t chroma_height = height / 2u;
            const std::size_t chroma_cells =
                static_cast<std::size_t>(chroma_width) * chroma_height;
            const uint8_t* const y_plane = nv12;
            const uint8_t* const uv_plane = nv12 + pixels;

            /* 0=背景，1=候选且未访问，2=已访问。 */
            mask_.assign(chroma_cells, 0u);
            uint32_t total_candidate_cells = 0;
            for(uint32_t cy = 0; cy < chroma_height; ++cy) {
                const std::size_t y0 = static_cast<std::size_t>(cy * 2u) * width;
                const std::size_t y1 = y0 + width;
                const std::size_t uv_row = static_cast<std::size_t>(cy) * width;
                const std::size_t mask_row =
                    static_cast<std::size_t>(cy) * chroma_width;
                for(uint32_t cx = 0; cx < chroma_width; ++cx) {
                    const std::size_t uv_idx = uv_row + cx * 2u;
                    const uint8_t u = uv_plane[uv_idx];
                    const uint8_t v = uv_plane[uv_idx + 1u];
                    if(v < kMinV ||
                       static_cast<int>(v) - static_cast<int>(u) <
                           kMinVMinusU) {
                        continue;
                    }

                    const std::size_t x = static_cast<std::size_t>(cx) * 2u;
                    const uint8_t local_y = std::max(
                        std::max(y_plane[y0 + x], y_plane[y0 + x + 1u]),
                        std::max(y_plane[y1 + x], y_plane[y1 + x + 1u]));
                    if(local_y < kMinLocalY) {
                        continue;
                    }
                    mask_[mask_row + cx] = 1u;
                    ++total_candidate_cells;
                }
            }

            if(total_candidate_cells == 0u) {
                out.reason = "no_red_pixels";
                finish(out);
                UPC_LOG_VISUAL_SERVICE(Debug,
                    "red dot detect seq=%u point=%u result=0 reason=%s ms=%.3f",
                    static_cast<unsigned>(frame_seq),
                    static_cast<unsigned>(hint.point_index), out.reason.c_str(),
                    out.total_ms);
                return out;
            }

            uint32_t best_cells = 0;
            uint64_t best_sum_x = 0;
            uint64_t best_sum_y = 0;
            uint64_t best_sum_v = 0;
            uint64_t best_sum_dominance = 0;
            uint64_t best_sum_y_peak = 0;

            stack_.clear();
            for(uint32_t cy = 0; cy < chroma_height; ++cy) {
                for(uint32_t cx = 0; cx < chroma_width; ++cx) {
                    const uint32_t seed = cy * chroma_width + cx;
                    if(mask_[seed] != 1u) {
                        continue;
                    }

                    uint32_t component_cells = 0;
                    uint64_t sum_x = 0;
                    uint64_t sum_y = 0;
                    uint64_t sum_v = 0;
                    uint64_t sum_dominance = 0;
                    uint64_t sum_y_peak = 0;
                    stack_.push_back(seed);
                    mask_[seed] = 2u;

                    while(!stack_.empty()) {
                        const uint32_t idx = stack_.back();
                        stack_.pop_back();
                        const uint32_t py = idx / chroma_width;
                        const uint32_t px = idx - py * chroma_width;
                        const std::size_t uv_idx =
                            static_cast<std::size_t>(py) * width + px * 2u;
                        const uint8_t u = uv_plane[uv_idx];
                        const uint8_t v = uv_plane[uv_idx + 1u];
                        const std::size_t full_x = static_cast<std::size_t>(px) * 2u;
                        const std::size_t full_y0 =
                            static_cast<std::size_t>(py * 2u) * width;
                        const std::size_t full_y1 = full_y0 + width;
                        const uint8_t y_peak = std::max(
                            std::max(y_plane[full_y0 + full_x],
                                     y_plane[full_y0 + full_x + 1u]),
                            std::max(y_plane[full_y1 + full_x],
                                     y_plane[full_y1 + full_x + 1u]));

                        ++component_cells;
                        sum_x += px;
                        sum_y += py;
                        sum_v += v;
                        sum_dominance += static_cast<uint32_t>(
                            static_cast<int>(v) - static_cast<int>(u));
                        sum_y_peak += y_peak;

                        const uint32_t min_y = py == 0u ? 0u : py - 1u;
                        const uint32_t max_y =
                            std::min(chroma_height - 1u, py + 1u);
                        const uint32_t min_x = px == 0u ? 0u : px - 1u;
                        const uint32_t max_x =
                            std::min(chroma_width - 1u, px + 1u);
                        for(uint32_t ny = min_y; ny <= max_y; ++ny) {
                            for(uint32_t nx = min_x; nx <= max_x; ++nx) {
                                const uint32_t neighbor =
                                    ny * chroma_width + nx;
                                if(mask_[neighbor] == 1u) {
                                    mask_[neighbor] = 2u;
                                    stack_.push_back(neighbor);
                                }
                            }
                        }
                    }

                    if(component_cells > best_cells ||
                       (component_cells == best_cells &&
                        sum_dominance > best_sum_dominance)) {
                        best_cells = component_cells;
                        best_sum_x = sum_x;
                        best_sum_y = sum_y;
                        best_sum_v = sum_v;
                        best_sum_dominance = sum_dominance;
                        best_sum_y_peak = sum_y_peak;
                    }
                }
            }

            out.area_px = static_cast<double>(best_cells) * 4.0;
            if(best_cells < kMinComponentCells) {
                out.reason = "area_too_small";
                finish(out);
                UPC_LOG_VISUAL_SERVICE(Debug,
                    "red dot detect seq=%u point=%u result=0 reason=%s area=%.0f candidates=%u ms=%.3f",
                    static_cast<unsigned>(frame_seq),
                    static_cast<unsigned>(hint.point_index), out.reason.c_str(),
                    out.area_px, static_cast<unsigned>(total_candidate_cells),
                    out.total_ms);
                return out;
            }

            const double cells = static_cast<double>(best_cells);
            out.center_x = 2.0 * (static_cast<double>(best_sum_x) / cells) + 1.0;
            out.center_y = 2.0 * (static_cast<double>(best_sum_y) / cells) + 1.0;

            const double mean_v = static_cast<double>(best_sum_v) / cells;
            const double mean_dominance =
                static_cast<double>(best_sum_dominance) / cells;
            const double mean_y_peak =
                static_cast<double>(best_sum_y_peak) / cells;
            const double area_score = clamp01(cells / 8.0);
            const double v_score = clamp01((mean_v - 128.0) / 80.0);
            const double dominance_score = clamp01(mean_dominance / 100.0);
            const double brightness_score =
                clamp01((mean_y_peak - kMinLocalY) /
                        (255.0 - kMinLocalY));
            out.confidence = clamp01(0.35 * area_score + 0.30 * v_score +
                                     0.25 * dominance_score +
                                     0.10 * brightness_score);
            out.found = true;
            out.reason = "detected";
            finish(out);
            UPC_LOG_VISUAL_SERVICE(Debug,
                "red dot detect seq=%u point=%u expect_mm=%.1f,%.1f result=1 px=%.1f,%.1f area=%.0f conf=%.3f candidates=%u ms=%.3f",
                static_cast<unsigned>(frame_seq),
                static_cast<unsigned>(hint.point_index),
                hint.expect_x_mm, hint.expect_y_mm,
                out.center_x, out.center_y, out.area_px, out.confidence,
                static_cast<unsigned>(total_candidate_cells), out.total_ms);
            return out;
        } catch(const std::exception& ex) {
            out = RedDotDetection{};
            out.reason = "internal_error";
            out.error = ex.what();
        } catch(...) {
            out = RedDotDetection{};
            out.reason = "internal_error";
            out.error = "unknown exception";
        }

        finish(out);
        UPC_LOG_VISUAL_SERVICE(Error,
            "red dot detect failed closed seq=%u point=%u error=%s ms=%.3f",
            static_cast<unsigned>(frame_seq),
            static_cast<unsigned>(hint.point_index), out.error.c_str(),
            out.total_ms);
        return out;
    }

    void reset()
    {
        /* 这里只清工作区内容；capacity 可复用，不携带任何跨帧判定语义。 */
        mask_.clear();
        stack_.clear();
    }

private:
    std::vector<uint8_t> mask_;
    std::vector<uint32_t> stack_;
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

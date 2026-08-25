#ifndef UPC_FIRST_LAYER_DETECTOR_HPP
#define UPC_FIRST_LAYER_DETECTOR_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace upc::visual {

struct FirstLayerDetection {
    bool passed = false;
    bool alignment_passed = false;
    bool segmentation_ran = false;
    bool cache_available = false;
    bool is_ood = false;
    double ready_probability = 0.0;
    double confidence = 0.0;
    double ood_distance = 0.0;
    double segmentation_ms = 0.0;
    double preprocessing_ms = 0.0;
    double fusion_ms = 0.0;
    double total_ms = 0.0;
    std::string raw_class = "not_ready_for_first_layer";
    std::string output_class = "uncertain";
    std::string cache_event = "not_run";
    std::string rejection_reasons;
    std::string error;
    uint32_t canonical_width = 0;
    uint32_t canonical_height = 0;
    std::vector<uint8_t> canonical_gray;
};

struct FirstLayerGeometryObservation {
    bool segmentation_ran = false;
    bool cache_available = false;
    double segmentation_ms = 0.0;
    std::string event = "not_run";
    std::string reason = "not_run";
};

/**
 * Cached PP-LiteSeg geometry and four-channel distilled EfficientNet detector.
 *
 * The input is a tight NV12 frame. PP-LiteSeg runs only while establishing or
 * auditing the cached homography. Classification uses the NV12 Y plane after
 * perspective rectification and illumination-robust texture preprocessing.
 * Any missing cache, model/runtime error, scene-quality rejection, low
 * confidence, or OOD embedding produces passed=false.
 */
class FirstLayerDetector {
public:
    FirstLayerDetector();
    ~FirstLayerDetector();

    FirstLayerDetector(const FirstLayerDetector&) = delete;
    FirstLayerDetector& operator=(const FirstLayerDetector&) = delete;

    /** Cheap scheduling check used by the Camera consumer thread. */
    bool geometry_observation_due(uint32_t frame_seq) const;

    /** Run one scheduled geometry observation. A valid old cache is retained on failure. */
    FirstLayerGeometryObservation observe_nv12(
        const uint8_t* nv12, std::size_t bytes,
        uint32_t width, uint32_t height, uint32_t frame_seq);

    /** Run the complete cached-geometry + fusion classification pipeline. */
    FirstLayerDetection predict_nv12(const uint8_t* nv12, std::size_t bytes,
                                     uint32_t width, uint32_t height,
                                     uint32_t frame_seq);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace upc::visual

#endif

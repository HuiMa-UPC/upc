#ifndef UPC_FIRST_LAYER_DETECTOR_HPP
#define UPC_FIRST_LAYER_DETECTOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace upc::visual {

namespace local_powder_gate {

/**
 * Evidence is measured on 14x14 cells, aggregated 2x2 into a 7x7 decision
 * grid.  The outside decision-cell ring is diagnostic-only because the plate
 * rim and the perspective warp are less stable there.
 */
inline constexpr uint32_t kEvidenceGridSize = 14;
inline constexpr uint32_t kDecisionGridSize = 7;
inline constexpr uint32_t kDecisionBorder = 1;
inline constexpr uint32_t kEvaluatedDecisionCells = 25;
inline constexpr uint32_t kMinCoveredDecisionCells = 20;
inline constexpr uint32_t kMaxConnectedBareDecisionCells = 3;
inline constexpr double kMinReferenceChangeMeanAbs = 4.0;
inline constexpr double kBareFineTextureCorrelation = 0.55;
inline constexpr double kMinPowderSignedChange = 32.0;

} // namespace local_powder_gate

struct FirstLayerDetection {
    bool passed = false;
    bool alignment_passed = false;
    bool segmentation_ran = false;
    bool cache_available = false;
    bool is_ood = false;
    double ready_probability = 0.0;
    double confidence = 0.0;
    double ood_distance = 0.0;
    bool local_powder_gate_passed = false;
    double reference_change_mean_abs = 0.0;
    double local_powder_coverage = 0.0;
    uint32_t local_bare_cells = 0;
    uint32_t local_bare_max_component = 0;
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
    std::vector<uint8_t> canonical_bare_gray;
    /** Exact uint8 representation of fusion input channel 5: log(A)-log(B0). */
    uint32_t reference_change_width = 0;
    uint32_t reference_change_height = 0;
    std::vector<uint8_t> reference_change_gray;
};

struct BareReferenceDiagnostic {
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
    std::string detail;
};

/**
 * Cached PP-LiteSeg geometry and five-channel reference-aware EfficientNet V3.
 *
 * The input is a tight NV12 frame. PP-LiteSeg runs only while establishing or
 * auditing the cached homography. Classification uses the NV12 Y plane after
 * perspective rectification and illumination-robust texture preprocessing.
 * The fifth channel is built from a bare reference captured in the same print
 * session before motion begins.  A missing reference always fails closed.
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

    /**
     * Capture the current tight NV12 frame as this print session's bare B0.
     * Geometry and scene quality must already be usable.  The raw Y plane is
     * retained so it can be warped with the same current homography as A.
     */
    bool capture_bare_reference_nv12(
        const uint8_t* nv12, std::size_t bytes,
        uint32_t width, uint32_t height, uint32_t frame_seq,
        std::string& reason, BareReferenceDiagnostic* diagnostic = nullptr);

    /** Clear B0 at print-session boundaries. */
    void clear_bare_reference();
    bool has_bare_reference() const;

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

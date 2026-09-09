#ifndef UPC_GENERATED_RKNN_CONTRACT_HPP
#define UPC_GENERATED_RKNN_CONTRACT_HPP

#include "media_paths.hpp"

#include <cstdint>

namespace rknn_first_layer_contract {

inline constexpr const char* kContractVersion =
    "ppliteseg_guarded_edge_snap_fusion_reference_v3_rknn_v13";
inline constexpr const char* kSegmentationPath =
    "/opt/first_layer_rknn/seg.rknn";
inline constexpr const char* kFusionPath =
    "/opt/first_layer_rknn/fusion.rknn";
inline constexpr const char* kCachePath =
    "/home/first_layer_rknn_geometry.yml";
inline constexpr const char* kQualifiedImageDir = UPC_FLV_QUALIFIED_DIR;
inline constexpr uint32_t kMaxQualifiedImages = 200;
inline constexpr const char* kSegmentationSha256 =
    "3222c50d46f76c8594b0355ebe3f4baacb65ae6de0e32e5895d719afc10525bf";
inline constexpr const char* kFusionSha256 =
    "a3f7295573288fde0976098ffe1228120025c6f54d081a45152d0a918844c358";

inline constexpr int kSegmentationSize = 320;
inline constexpr int kCanonicalSize = 392;
inline constexpr float kInsetScale = 1.02f;
inline constexpr int kInitialValidFrames = 10;
inline constexpr int kProposalValidFrames = 5;
inline constexpr uint32_t kAuditIntervalFrames = 60;
inline constexpr uint32_t kFailedRetryFrames = 10;
inline constexpr uint32_t kDarkSceneRetryFrames = 60;
inline constexpr float kSegmentationMinGrayMean = 18.0f;
inline constexpr uint32_t kMaxAuditSpanFrames = 60;
inline constexpr float kAreaRatioMin = 0.15f;
inline constexpr float kAreaRatioMax = 0.27f;
inline constexpr float kCachedAreaRelativeTolerance = 0.05f;
inline constexpr int kGeometryCloseKernel = 5;
inline constexpr float kGeometryCentralRoiMin = 0.30f;
inline constexpr float kGeometryCentralRoiMax = 0.70f;
inline constexpr float kGeometryEdgeTrimFraction = 0.12f;
inline constexpr float kGeometryEdgeInlierDistance320 = 6.0f;
inline constexpr float kGeometryEdgeInlierFractionMin = 0.50f;
inline constexpr float kGeometryEdgeResidualMax320 = 6.0f;
inline constexpr float kGeometryMaskCoverageMin = 0.72f;
inline constexpr float kGeometryQuadFillMin = 0.82f;
inline constexpr float kGeometryOppositeEdgeLengthRatioMin = 0.60f;
inline constexpr int kGeometryEdgeSnapSearchRadius320 = 16;
inline constexpr int kGeometryEdgeSnapSideSearchRadius320 = 64;
inline constexpr float kGeometryEdgeSnapTrimFraction = 0.15f;
inline constexpr float kGeometryEdgeSnapMinNormalGradient = 5.0f;
inline constexpr float kGeometryEdgeSnapMinContinuity = 0.40f;
inline constexpr float kGeometryEdgeSnapMinScore = 8.0f;
inline constexpr float kGeometryEdgeSnapDistancePenalty = 0.10f;
inline constexpr float kGeometryEdgeSnapSideMinNormalGradient = 2.0f;
inline constexpr float kGeometryEdgeSnapSideMinContinuity = 0.15f;
inline constexpr float kGeometryEdgeSnapSideMinScore = 3.0f;
inline constexpr float kGeometryEdgeSnapSideDistancePenalty = 0.03f;
inline constexpr float kGeometryEdgeSnapAreaRatioMin = 0.70f;
inline constexpr float kGeometryEdgeSnapAreaRatioMax = 1.45f;
inline constexpr float kGeometryEdgeSnapMaxCornerDelta320 = 8.0f;
inline constexpr float kComponentFractionMin = 0.70f;
inline constexpr float kMeanProbabilityMin = 0.80f;
inline constexpr float kNoMoveTranslation320 = 4.0f;
inline constexpr float kNoMoveMaxCorner320 = 8.0f;
inline constexpr float kMoveTranslation320 = 5.0f;
inline constexpr float kMoveMaxCorner320 = 10.0f;
inline constexpr float kProposalAgreement320 = 3.0f;
inline constexpr float kGroupScatterMax320 = 12.5f;

} // namespace rknn_first_layer_contract

#endif

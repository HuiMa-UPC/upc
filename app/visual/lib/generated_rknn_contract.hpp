#ifndef UPC_GENERATED_RKNN_CONTRACT_HPP
#define UPC_GENERATED_RKNN_CONTRACT_HPP

#include "media_paths.hpp"

#include <cstdint>

namespace rknn_first_layer_contract {

inline constexpr const char* kContractVersion =
    "ppliteseg_cached_geometry_fusion_reference_v3_rknn_v1";
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
inline constexpr float kInsetScale = 1.0f;
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
inline constexpr float kComponentFractionMin = 0.95f;
inline constexpr float kQuadIouMin = 0.91f;
inline constexpr float kMeanProbabilityMin = 0.80f;
inline constexpr float kNoMoveTranslation320 = 4.0f;
inline constexpr float kNoMoveMaxCorner320 = 8.0f;
inline constexpr float kMoveTranslation320 = 5.0f;
inline constexpr float kMoveMaxCorner320 = 10.0f;
inline constexpr float kProposalAgreement320 = 3.0f;
inline constexpr float kGroupScatterMax320 = 12.5f;

} // namespace rknn_first_layer_contract

#endif

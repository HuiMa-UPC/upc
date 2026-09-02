#ifndef MEDIA_DEVICE_CONFIG_HPP
#define MEDIA_DEVICE_CONFIG_HPP

#include "config/camera_resolution_profile.hpp"

/** 录像默认几何（运行时以 Camera 开流 IPC 为准；此处仅成员初值） */
#ifndef CAMERA_WIDTH_DEFAULT
#define CAMERA_WIDTH_DEFAULT  UPC_CAMERA_STREAM_WIDTH
#endif

#ifndef CAMERA_HEIGHT_DEFAULT
#define CAMERA_HEIGHT_DEFAULT UPC_CAMERA_STREAM_HEIGHT
#endif

#ifndef CAMERA_FPS_DEFAULT
#define CAMERA_FPS_DEFAULT  30u
#endif

#include "media_paths.hpp"

#ifndef CAMERA_PRINT_VIDEO_OUT_DIR
#define CAMERA_PRINT_VIDEO_OUT_DIR  UPC_VIDEO_DIR
#endif

/** 延时摄影缩略图后缀（RTHM 原始 RGB565，UI 自定义 decoder 读取） */
#ifndef CAMERA_PRINT_VIDEO_THUMB_EXT
#define CAMERA_PRINT_VIDEO_THUMB_EXT  ".rthm"
#endif

/**
 * 缩略图最大宽度（px）：抽帧时按此宽度等比缩小，避免 UI 加载原生大分辨率
 * JPG（如 1536×1920 全解 ≈11.8MB/张）。高度由 ffmpeg -1 自动等比。
 */
#ifndef CAMERA_PRINT_VIDEO_THUMB_MAX_W
#define CAMERA_PRINT_VIDEO_THUMB_MAX_W  240
#endif

/** 板端 ffmpeg 可执行文件（rootfs 需已部署） */
#ifndef CAMERA_FFMPEG_BIN
#define CAMERA_FFMPEG_BIN  "/usr/bin/ffmpeg"
#endif

/** 板端 MPP 编码测试工具（用于 NV12 -> H264 裸流） */
#ifndef CAMERA_MPI_ENC_BIN
#define CAMERA_MPI_ENC_BIN  "/usr/bin/mpi_enc_test"
#endif

/**
 * H264 目标码率（bps）：打印期连续录像与直播共用。
 * 录像可被 cal.csv PRINT_H264_BITRATE 覆盖；直播固定用本值，
 * 不受云端 max_bitrate_kbps 影响。
 */
#ifndef CAMERA_PRINT_H264_BITRATE
#define CAMERA_PRINT_H264_BITRATE  2048000u
#endif

/** 层帧率：UI 延时摄影序列播放默认帧率；合成默认亦为此值（可被 cal.csv TIMELAPSE_VIDEO_FPS 覆盖） */
#ifndef CAMERA_PRINT_VIDEO_LAYER_FPS
#define CAMERA_PRINT_VIDEO_LAYER_FPS  5u
#endif

/** 存储已用 >= 此百分比：本会话停 H264、跳过层间拍照 */
#ifndef MEDIA_STORAGE_WARN_PCT
#define MEDIA_STORAGE_WARN_PCT  90u
#endif

/** 存储已用 >= 此百分比：强制 GC 临时区与失败半成品 */
#ifndef MEDIA_STORAGE_CRITICAL_PCT
#define MEDIA_STORAGE_CRITICAL_PCT  95u
#endif

#endif /* MEDIA_DEVICE_CONFIG_HPP */

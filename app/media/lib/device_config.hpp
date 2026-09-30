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

/**
 * 相册拍照 / 延时层拍 / 清粉抽帧的 JPEG 质量（1..100）。
 * CPU libturbojpeg，不占 RKMPP（H.264 录像与直播仍走硬件）。
 */
#ifndef CAMERA_STILL_JPEG_QUALITY
#define CAMERA_STILL_JPEG_QUALITY  80
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
 * 打印期连续 H264 目标码率（bps）。固定用本值，不再读 cal.csv。
 * 直播用 UPC_LIVE_H264_BITRATE，不受云端 max_bitrate_kbps 影响。
 */
#ifndef CAMERA_PRINT_H264_BITRATE
#define CAMERA_PRINT_H264_BITRATE  2048000u
#endif

/**
 * 直播（局域网 RTSP / 云端推流）目标码率（bps）。
 * 与打印录像分开：1080×1280@30 用 2Mbps 且 GOP=0.5s 时 I 帧会吃光预算，
 * 运动发糊、播放器为抹时间戳抖动又加缓冲，观感就是「不流畅」。
 */
#ifndef UPC_LIVE_H264_BITRATE
#define UPC_LIVE_H264_BITRATE  4096000u
#endif

/** 层帧率：延时抽帧/合成与序列播放默认帧率；不再读 cal.csv TIMELAPSE_VIDEO_FPS */
#ifndef CAMERA_PRINT_VIDEO_LAYER_FPS
#define CAMERA_PRINT_VIDEO_LAYER_FPS  5u
#endif

/**
 * 逐帧写盘前存储预算检查的缓存窗口（ms）。
 *
 * 全量核算要递归遍历整个录像目录并 stat 每个文件，而层拍 JPEG 会累积到上千个，
 * 按帧调用时开销随打印层数线性增长，足以把 H264 编码 worker 拖到丢一半帧。
 * 配额本身是"分区容量 80%"这种粗水位，秒级刷新完全够用。
 */
#ifndef UPC_MEDIA_STORAGE_BUDGET_CACHE_MS
#define UPC_MEDIA_STORAGE_BUDGET_CACHE_MS  5000u
#endif

/**
 * finalize 时等待封装进程写完 mp4 尾部的上限（ms）。
 *
 * 关掉 stdin 后 ffmpeg 读到 EOF 还要写 moov 索引，几万帧时远超百毫秒级。
 * 等不够就强杀会留下没有 moov、播放器打不开的 mp4。
 */
#ifndef UPC_PRINT_H264_MUX_FLUSH_TIMEOUT_MS
#define UPC_PRINT_H264_MUX_FLUSH_TIMEOUT_MS  10000u
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

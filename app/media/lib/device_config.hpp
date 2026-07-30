#ifndef MEDIA_DEVICE_CONFIG_HPP
#define MEDIA_DEVICE_CONFIG_HPP

/** 录像默认几何（运行时以 Camera 开流 IPC 为准；此处仅成员初值） */
#ifndef CAMERA_WIDTH_DEFAULT
#define CAMERA_WIDTH_DEFAULT  1024u
#endif

#ifndef CAMERA_HEIGHT_DEFAULT
#define CAMERA_HEIGHT_DEFAULT 1280u
#endif

#ifndef CAMERA_FPS_DEFAULT
#define CAMERA_FPS_DEFAULT  30u
#endif

#ifndef CAMERA_PRINT_VIDEO_OUT_DIR
#define CAMERA_PRINT_VIDEO_OUT_DIR  "/home/video"
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

/** 打印期连续 H264 默认码率（bps） */
#ifndef CAMERA_PRINT_H264_BITRATE
#define CAMERA_PRINT_H264_BITRATE  4000000u
#endif

/** 层帧率：UI 延时摄影序列播放默认帧率；合成默认亦为此值（可被 cal.csv TIMELAPSE_VIDEO_FPS 覆盖） */
#ifndef CAMERA_PRINT_VIDEO_LAYER_FPS
#define CAMERA_PRINT_VIDEO_LAYER_FPS  5u
#endif

#endif /* MEDIA_DEVICE_CONFIG_HPP */

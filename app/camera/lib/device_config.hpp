#ifndef CAMERA_DEVICE_CONFIG_HPP
#define CAMERA_DEVICE_CONFIG_HPP

/** V4L2 主视频节点（环境变量 UPC_CAMERA_DEVICE 可覆盖） */
#ifndef CAMERA_V4L2_DEVICE_DEFAULT
#define CAMERA_V4L2_DEVICE_DEFAULT  "/dev/video0"
#endif

#ifndef CAMERA_ENV_DEVICE
#define CAMERA_ENV_DEVICE  "UPC_CAMERA_DEVICE"
#endif

#ifndef CAMERA_WIDTH_DEFAULT
#define CAMERA_WIDTH_DEFAULT  1024u
#endif

#ifndef CAMERA_HEIGHT_DEFAULT
#define CAMERA_HEIGHT_DEFAULT 1280u
#endif

#ifndef CAMERA_FPS_DEFAULT
#define CAMERA_FPS_DEFAULT  30u
#endif

#ifndef CAMERA_ENV_FPS
#define CAMERA_ENV_FPS  "UPC_CAMERA_FPS"
#endif

/**
 * 开流后前 N 帧只喂 ISP/AE，不 commit 到 SHM，避免预览首帧由暗变亮。
 * 环境变量 UPC_CAMERA_WARMUP_FRAMES 可覆盖（0=关闭）。
 */
#ifndef CAMERA_WARMUP_FRAMES_DEFAULT
#define CAMERA_WARMUP_FRAMES_DEFAULT  12u
#endif

#ifndef CAMERA_ENV_WARMUP_FRAMES
#define CAMERA_ENV_WARMUP_FRAMES  "UPC_CAMERA_WARMUP_FRAMES"
#endif

/** ISP 校准文件目录与主 json */
#ifndef CAMERA_IQ_DIR_DEFAULT
#define CAMERA_IQ_DIR_DEFAULT  "/home"
#endif

#ifndef CAMERA_IQ_FILE_DEFAULT
#define CAMERA_IQ_FILE_DEFAULT  "wpcam_NC_NC.json"
#endif

#ifndef CAMERA_ENV_IQ_DIR
#define CAMERA_ENV_IQ_DIR  "UPC_CAMERA_IQ_DIR"
#endif

#ifndef CAMERA_ENV_IQ_FILE
#define CAMERA_ENV_IQ_FILE  "UPC_CAMERA_IQ_FILE"
#endif

/** 传感器 media entity（空则自动探测；失败时用 FALLBACK） */
#ifndef CAMERA_ENV_SENSOR_ENTITY
#define CAMERA_ENV_SENSOR_ENTITY  "UPC_CAMERA_SENSOR_ENTITY"
#endif

/** 传感器 subdev（曝光/增益等，可选） */
#ifndef CAMERA_SENSOR_SUBDEV_DEFAULT
#define CAMERA_SENSOR_SUBDEV_DEFAULT  "/dev/v4l-subdev2"
#endif

#ifndef CAMERA_SENSOR_ENTITY_FALLBACK
#define CAMERA_SENSOR_ENTITY_FALLBACK  "m00_b_wpcam"
#endif

/** V4L2 采集缓冲个数 */
#ifndef CAMERA_V4L2_BUF_COUNT
#define CAMERA_V4L2_BUF_COUNT  4u
#endif

/** SHM 帧环（与 UI CameraPreviewPlayer 约定一致） */
#ifndef CAMERA_SHM_NAME_DEFAULT
#define CAMERA_SHM_NAME_DEFAULT "/upc_camera_preview"
#endif

#ifndef CAMERA_SHM_SLOT_COUNT
#define CAMERA_SHM_SLOT_COUNT 3u
#endif

/** NV12 Y 平面 stride 对齐要求（RGA 通常要求 16/64 字节对齐） */
#ifndef CAMERA_STRIDE_ALIGN
#define CAMERA_STRIDE_ALIGN  16u
#endif

/** 0=纯 SHM poll；1=每帧额外发 APP_Camera_Frame_Ntf */
#ifndef CAMERA_FRAME_NTF_DEFAULT
#define CAMERA_FRAME_NTF_DEFAULT 0
#endif

#ifndef CAMERA_ENV_FRAME_NTF
#define CAMERA_ENV_FRAME_NTF "UPC_CAMERA_FRAME_NTF"
#endif

/** 打印录像：临时 H.264 目录与会话子目录前缀 */
#ifndef CAMERA_PRINT_VIDEO_TMP_DIR
#define CAMERA_PRINT_VIDEO_TMP_DIR  "/tmp/print_vid"
#endif

#ifndef CAMERA_PRINT_VIDEO_OUT_DIR
#define CAMERA_PRINT_VIDEO_OUT_DIR  "/home/video"
#endif

/** 输出文件后缀（MPP 输出 Annex-B H.264  elementary stream，非 MP4 容器） */
#ifndef CAMERA_PRINT_VIDEO_OUT_EXT
#define CAMERA_PRINT_VIDEO_OUT_EXT  ".h264"
#endif

/** 写入 H.264 时标注的层帧率（仅 metadata 用途；raw .h264 无容器时间轴） */
#ifndef CAMERA_PRINT_VIDEO_LAYER_FPS
#define CAMERA_PRINT_VIDEO_LAYER_FPS  5u
#endif

#endif /* CAMERA_DEVICE_CONFIG_HPP */

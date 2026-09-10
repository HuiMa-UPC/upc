#ifndef CAMERA_DEVICE_CONFIG_HPP
#define CAMERA_DEVICE_CONFIG_HPP

#include "config/camera_resolution_profile.hpp"

/** V4L2 主视频节点（环境变量 UPC_CAMERA_DEVICE 可覆盖） */
#ifndef CAMERA_V4L2_DEVICE_DEFAULT
#define CAMERA_V4L2_DEVICE_DEFAULT  "/dev/video0"
#endif

#ifndef CAMERA_ENV_DEVICE
#define CAMERA_ENV_DEVICE  "UPC_CAMERA_DEVICE"
#endif

#ifndef CAMERA_WIDTH_DEFAULT
#define CAMERA_WIDTH_DEFAULT  UPC_CAMERA_STREAM_WIDTH
#endif

#ifndef CAMERA_HEIGHT_DEFAULT
#define CAMERA_HEIGHT_DEFAULT UPC_CAMERA_STREAM_HEIGHT
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

/**
 * V4L2 采集缓冲个数。
 *
 * 需同时供三份：驱动侧待填充、采集线程当前处理、已下发给消费者尚未归还
 * （见 CAMERA_MAX_FRAMES_IN_FLIGHT）。stride 不对齐时 padding pool 按同样
 * 个数分配，因此这里每 +1 约多占两倍单帧 dma 内存。
 */
#ifndef CAMERA_V4L2_BUF_COUNT
#define CAMERA_V4L2_BUF_COUNT  6u
#endif

/**
 * 允许同时"在飞"（已 commit 给消费者、buffer 尚未归还）的帧数上限。
 *
 * 采集线程不再逐帧同步等待消费者归还，而是把已提交的帧挂进在飞队列，
 * 每轮先非阻塞回收已完成的，只在队列达到本上限时才阻塞等最老一帧。
 * 这样单个消费者的瞬时抖动被窗口吸收，不会把整条采集链路（以及依赖它的
 * 直播编码）拖到该消费者的速率。
 *
 * 实际窗口还会被 buffer 数（留 2 个给驱动填充）与 SHM 槽位数（留 1 个给
 * 写入）夹住，取三者最小值。3 帧 @30fps 约可吸收 100ms 抖动。
 */
#ifndef CAMERA_MAX_FRAMES_IN_FLIGHT
#define CAMERA_MAX_FRAMES_IN_FLIGHT  3u
#endif

/** SHM 帧环（与 UI CameraPreviewPlayer 约定一致） */
#ifndef CAMERA_SHM_NAME_DEFAULT
#define CAMERA_SHM_NAME_DEFAULT "/upc_camera_preview"
#endif

#ifndef CAMERA_SHM_SLOT_COUNT
/** 元数据环槽位数：打印 Snap 与 UI 预览双 Consumer 时 3 槽易饿死，提高到 6 缓解背压 */
#define CAMERA_SHM_SLOT_COUNT 6u
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

/**
 * 采集看门狗：成功出帧后超过该毫秒仍连续 dequeue 失败，则原地重启 ISP/V4L2，
 * 并给现有 peers 重推 Start Rsp（新 dma fd）。0=关闭。
 */
#ifndef CAMERA_STREAM_WATCHDOG_STALL_MS
#define CAMERA_STREAM_WATCHDOG_STALL_MS  3000u
#endif

/** 两次看门狗重启之间的最小间隔（毫秒），避免抖振狂重启 */
#ifndef CAMERA_STREAM_WATCHDOG_COOLDOWN_MS
#define CAMERA_STREAM_WATCHDOG_COOLDOWN_MS  8000u
#endif

#endif /* CAMERA_DEVICE_CONFIG_HPP */

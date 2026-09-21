#ifndef UPC_MEDIA_PATHS_HPP
#define UPC_MEDIA_PATHS_HPP

/**
 * 本机媒体落盘根路径（拍照 / 延时摄影 / 打印录像）。
 *
 *   /userdata/media/
 *     photo/                 相册 JPEG + 配对 .rthm
 *     video/
 *       config.json          打印历史清单
 *       tmp/                 唯一打印中临时目录（开打前/开机可清空）
 *       {job}/               正式会话产物
 *     first_layer_visual/    首层视觉诊断图（qualified / rejected）
 *
 * Media / UI / 清单解析共用此头，避免各进程硬编码分叉。
 */

#ifndef UPC_MEDIA_ROOT
#define UPC_MEDIA_ROOT  "/userdata/media"
#endif

#ifndef UPC_PHOTO_DIR
#define UPC_PHOTO_DIR  UPC_MEDIA_ROOT "/photo"
#endif

#ifndef UPC_VIDEO_DIR
#define UPC_VIDEO_DIR  UPC_MEDIA_ROOT "/video"
#endif

/** 打印中唯一临时目录名（相对 UPC_VIDEO_DIR；亦作清单 frames_dir 快路径主键） */
#ifndef UPC_VIDEO_TMP_NAME
#define UPC_VIDEO_TMP_NAME  "tmp"
#endif

#ifndef UPC_VIDEO_TMP
#define UPC_VIDEO_TMP  UPC_VIDEO_DIR "/" UPC_VIDEO_TMP_NAME
#endif

/** 打印历史（延时 + 可选打印录像）清单；相对路径均相对 UPC_VIDEO_DIR */
#ifndef UPC_VIDEO_CONFIG
#define UPC_VIDEO_CONFIG  UPC_VIDEO_DIR "/config.json"
#endif

/** 相册清单（可选；当前 Album 仍可扫目录，路径预留给后续） */
#ifndef UPC_PHOTO_CONFIG
#define UPC_PHOTO_CONFIG  UPC_PHOTO_DIR "/config.json"
#endif

/** 首层视觉诊断图（相对 UPC_MEDIA_ROOT） */
#ifndef UPC_FLV_DIR
#define UPC_FLV_DIR  UPC_MEDIA_ROOT "/first_layer_visual"
#endif

#ifndef UPC_FLV_QUALIFIED_DIR
#define UPC_FLV_QUALIFIED_DIR  UPC_FLV_DIR "/qualified"
#endif

#ifndef UPC_FLV_REJECTED_DIR
#define UPC_FLV_REJECTED_DIR  UPC_FLV_DIR "/rejected"
#endif

/**
 * 延时摄影播放形态（Media + UI 共用编译开关；宏名保留 PNG，实际帧是 JPEG）：
 *   1 = 静图序列播放（默认）：开门 finalize 不编 timelapse.mp4，保留层间 1.jpg…；
 *       打印录像（print_h264.mp4）仍可按开关 remux。
 *   0 = MP4 播放：开门后后台 JPEG→timelapse.mp4，成功后可删清粉段 JPEG。
 * 可用 -DUPC_TIMELAPSE_PLAYBACK_PNG=0 覆盖。
 */
#ifndef UPC_TIMELAPSE_PLAYBACK_PNG
#define UPC_TIMELAPSE_PLAYBACK_PNG  1
#endif

/**
 * 录像区环形存储预算：
 * - 总预算按录像所在分区容量的百分比动态计算，避免写死设备标称容量；
 * - 余量用于系统、日志与封装期间 H264/MP4 双份文件并存。
 */
#ifndef UPC_VIDEO_STORAGE_QUOTA_PCT
#define UPC_VIDEO_STORAGE_QUOTA_PCT  80u
#endif

#ifndef UPC_VIDEO_STORAGE_MIN_FREE_PCT
#define UPC_VIDEO_STORAGE_MIN_FREE_PCT  5u
#endif

/* ---- 兼容旧宏名（逐步迁移调用方） ---- */
#ifndef UPC_PRINT_VIDEO_DIR
#define UPC_PRINT_VIDEO_DIR  UPC_VIDEO_DIR
#endif

#ifndef UPC_PRINT_VIDEO_MANIFEST
#define UPC_PRINT_VIDEO_MANIFEST  UPC_VIDEO_CONFIG
#endif

#endif /* UPC_MEDIA_PATHS_HPP */

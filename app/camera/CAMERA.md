# camera/ — 相机后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 采集进程：**`rongsu_camera`**（仅采集/推流）  
> 媒体进程：**`rongsu_media`**（拍照/录像/延时封装；复用本目录下 recorder 源）

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | IPC Server、`CameraService` 生命周期、`process(2)` 循环 |
| `lib/camera_service.*` | 流控 IPC、dma fd 元数据环推流、录像消息分派 |
| `lib/vi_capture.*` | rkaiq ISP + V4L2 NV12 采集 |
| `../media/lib/print_video_recorder.*` | 元数据环取帧 → ffmpeg 软编 PNG → `/home/video/{job_ts}/1.png...` + `.rthm` |
| `../media/lib/print_h264_recorder.*` | 预览 NV12 异步入队写 raw（队列满丢帧保预览）；结束/终止后 worker 转码 `print.h264` |
| `../media/lib/clean_video_recorder.*` | 清粉延时抽帧：按 `TIMELAPSE_VIDEO_FPS` 写接续 PNG，成功后重建 `timelapse.mp4`+`thumb.rthm` 并删除清粉 PNG |
| `lib/device_config.hpp` | 设备、分辨率、IQ、SHM、录像路径宏 |

## 数据路径

### 预览

真实相机路径使用 **dma fd 旁路**：

```
Sensor → rkaiq ISP → /dev/video0 (NV12)
  → ViCapture VIDIOC_EXPBUF 导出 dma_buf fd
  → CameraService 推流线程：
      · 经主 IPC（/tmp/upc_camera.sock）SCM_RIGHTS 一次性把所有 fd 发给 UI Hub
      · SHM 元数据环按消费者拆分：UI `/upc_camera_preview`、Visual `/upc_camera_preview_visual`
        （只保留 slot 状态/seq/buffer_idx，不再存像素）
  → UI CameraPreviewPlayer：
      · MsgPort::take_peer_dma_fds(APP_CAMERA) 接收 fd 表
      · 按 buffer_idx 找到 dma_buf fd
      · RGA importbuffer_fd(src_fd) 直接做 NV12→BGRX（零拷贝源）
      · 失败时 mmap 源 fd 做软件回退
```

- 若 V4L2 输出 stride 不满足 RGA 对齐要求，后端把数据拷贝到 `CameraDmaFrameSource` 内部的对齐 dma_buf，再交给 UI。
- UI 不再分配中间 NV12 staging；源 fd 直接来自 V4L2 或后端 padding buffer。
- 状态机：后端 `DQBUF` 后把 `buffer_idx` 写入 `EMPTY` slot 并 `commit` 为 `READY`；UI `acquire_latest_frame` 取到 `buffer_idx` 后查 fd 表，RGA/SW 完成后 `release_slot` 置 `EMPTY`；后端 `wait_slot_empty` 等到 `EMPTY` 才把对应 V4L2 buffer `QBUF` 回去。
- `PrintVideoRecorder` 也改为通过 fd + mmap 读取最新帧。
- 参考 `doc/rv1106` ISP 初始化；当前无 Rockit VENC，不经 MPI 预览。

### 打印录像

```
Glavo Snap IPC → PrintVideoRecorder worker
  → `IpcDmaFrameRingConsumer::acquire_latest_frame` 拿到 `buffer_idx`
  → 通过 fd 回调 `mmap` 取 NV12（如 stride 有 padding 会按 active width 解压）
  → ffmpeg 软编 PNG（连续编号 1.png,2.png,...）
      → /home/video/{job}_{YYYYMMDD_HHMMSS}/
  → finish/abort
  → 缩略图：dma32 + RGA importbuffer_fd 生成 thumb.rthm（失败则无缩略图）
  → upsert /home/video/manifest.json（status=complete|terminated）
  → APP_Print_Video_Manifest_Ntf → UI（切打印结果页）
```

0 帧时不生成层间产物、不写 manifest，但**仍会发** `APP_Print_Video_Manifest_Ntf`
（`has_entry=0` + `aborted` 标志），否则 UI 会永远停在打印中界面下不来。
此时**不** `remove_all` 会话目录，以免误删同目录下正在后台转码的 `print_raw.nv12` / `print.h264`。

### 打印连续 H264（调试开关）

```
预览帧 → 推流线程：拷贝 NV12 入队（队列满丢帧，不 fwrite）
  → worker：顺序 append print_raw.nv12
  → Finish/Abort：active=0 + 入队 Encode（消息线程立刻返回）
  → worker：排空写帧后 mpi_enc → print.h264，成功后删 raw
```

与层间 `PrintVideoRecorder` worker 并行；结果页仍只等层间 Manifest，不依赖 H264 编完。
磁盘跟不上时录像可能丢帧（日志 `print_h264 queue full drop=...`），优先保证预览不卡死。

`manifest.json` 由 Camera 独写；字段含 `job_name` / `start_unix` / `duration_sec` / `material_g` / `status` / `frames_dir` / `thumb`。UI 历史页直读该文件（解析见 `common/fs_manifest/print_history_manifest.*`）。

Snap 依赖预览流已开（同一 SHM）；与 UI 解码线程共享 ring，偶发 `snap miss` 见日志。

## IPC / SHM

- **Server**：`set_mode_ipc_server(APP_CAMERA)`，`/tmp/upc_camera.sock`
- **预览**：`UI_Camera_Stream_*` / `APP_Camera_Stream_*`

| MsgType | 处理方动作 |
|---------|----------------------|
| `UI_Camera_Stream_Start_Req` | UI 开采集 + 导出 dma fd + 建 UI SHM 状态环 + 启动推流线程 |
| `UI_Camera_Stream_Stop_Req` | UI 停流（仅移除 UI 会话；无其他消费者时停相机） |
| `VISUAL_Camera_Stream_Start_Req` | Visual 开并行会话 + 建 Visual SHM 状态环 |
| `VISUAL_Camera_Stream_Stop_Req` | Visual 停流（仅移除 Visual 会话） |
| `MEDIA_Camera_Stream_Start_Req` | Media 并行取流（独立 SHM `/upc_camera_preview_media`） |
| `MEDIA_Camera_Stream_Stop_Req` | Media 停流（仅移除 Media 会话） |
| `UI_Camera_Snapshot_Req` | **Media** 保存当前预览帧 PNG + 配对 `.rthm` 缩略图到 `/home/photo` |
| `APP_Print_Video_Session_*` | **Media** 处理打印录像会话、生成缩略图/MP4/manifest |
| `APP_Clean_Video_Session_*` | **Media** 处理清粉延时抽帧（接续 PNG → 合并 `timelapse.mp4`） |
| `APP_Print_Video_Manifest_Ntf` | **Media→UI**：manifest `rename()` 成功后发（0 帧也必发） |

Glavo→Media 经 Broker 转发；`sender_id` 仍为 `APP_GLAVO`（见 IPC.md）。

可选：`APP_Camera_Frame_Ntf`（`CAMERA_FRAME_NTF_DEFAULT`，UI 预览走元数据环轮询时通常可关）。

## 默认参数

| 项 | 默认 | 环境变量 / 宏 |
|----|------|---------------|
| 设备 | `/dev/video0` | `UPC_CAMERA_DEVICE` |
| 分辨率 | 1080×1280 | `CAMERA_WIDTH/HEIGHT_DEFAULT` |
| 帧率 | 30 | `UPC_CAMERA_FPS` |
| IQ | `/home/wpcam_NC_NC.json` | `UPC_CAMERA_IQ_DIR` / `UPC_CAMERA_IQ_FILE` |
| V4L2 缓冲 | 4 | `CAMERA_V4L2_BUF_COUNT` |
| SHM 槽位 | 6 | `CAMERA_SHM_SLOT_COUNT`（UI/Snap 双消费；过小易 `slot release timeout`） |
| 录像输出 | `/home/video/{job}_{YYYYMMDD_HHMMSS}/{1..N}.png` | `CAMERA_PRINT_VIDEO_OUT_DIR` |
| 缩略图 | `/home/video/{job}_{YYYYMMDD_HHMMSS}/thumb.rthm` | `CAMERA_PRINT_VIDEO_THUMB_EXT` |
| 缩略图最大宽 | 240 | `CAMERA_PRINT_VIDEO_THUMB_MAX_W` |
| 拍照原图 | `/home/photo/photo_{stamp}_{WxH}.png` | Media `save_latest_png_snapshot` |
| 拍照缩略图 | `/home/photo/photo_{stamp}_{WxH}.rthm`（同帧 NV12→dma32+RGA） | 同上；失败不影响 PNG |
| 历史清单 | `/home/video/manifest.json` | `UPC_PRINT_VIDEO_MANIFEST` |
| ffmpeg | `/usr/bin/ffmpeg` | `CAMERA_FFMPEG_BIN` |

SHM 元数据环：`stride_y` 为 V4L2 实际 Y stride（dma 模式），`buffer_idx` 指向 fd 表；像素走 dma fd。

## 库文件一览

| 文件 | 职责 |
|------|------|
| `lib/camera_dma_frame_source.*` | V4L2 dma fd 导出、stride padding；开流经主 IPC SCM_RIGHTS 传 fd |
| `lib/device_config.hpp` | 设备、分辨率、IQ、SHM、录像路径宏 |

## 红线

- 禁止链接 `ui::port`、`upc::glavo`、lvgl
- 改消息契约：[common/COMMON.md](../../common/COMMON.md) 四步

## 相关文档

- [common/COMMON.md](../../common/COMMON.md) · [ui/UI.md](../ui/UI.md) · [glavo/GLAVO.md](../glavo/GLAVO.md)

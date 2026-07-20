# camera/ — 相机后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_camera`** · 库源在 `lib/`，**无独立静态库**（源直接编入可执行文件）

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | IPC Server、`CameraService` 生命周期、`process(2)` 循环 |
| `lib/camera_service.*` | 流控 IPC、dma fd 元数据环推流、录像消息分派 |
| `lib/vi_capture.*` | rkaiq ISP + V4L2 NV12 采集 |
| `lib/print_video_recorder.*` | 元数据环 + dma fd 取帧 → MPP H.264 → ffmpeg remux `.mp4` + 末帧 `.jpg` |
| `lib/device_config.hpp` | 设备、分辨率、IQ、SHM、录像路径宏 |

## 数据路径

### 预览

真实相机路径使用 **dma fd 旁路**：

```
Sensor → rkaiq ISP → /dev/video0 (NV12)
  → ViCapture VIDIOC_EXPBUF 导出 dma_buf fd
  → CameraService 推流线程：
      · 经主 IPC（/tmp/upc_camera.sock）SCM_RIGHTS 一次性把所有 fd 发给 UI Hub
      · SHM /upc_camera_preview 是 `IpcDmaFrameRing` 元数据环，只保留 slot 状态/seq/buffer_idx（不再存像素）
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
  → MPP encode → /tmp/print_vid/{session}/layers.h264
  → finish/abort → EOS flush
  → ffmpeg remux（-c copy）→ /home/video/{job}_{YYYYMMDD_HHMMSS}.mp4
  → ffmpeg 抽末帧 → /home/video/{job}_{YYYYMMDD_HHMMSS}.jpg
  → upsert /home/video/manifest.json（status=complete|terminated）
  → 删除临时 .h264 / session 目录
```

0 帧时不生成 `/home/video` 产物、不写 manifest。remux 失败时保留临时 `.h264` 便于排查。

`manifest.json` 由 Camera 独写；字段含 `job_name` / `start_unix` / `duration_sec` / `material_g`（读 Gcode config）/ `status` / `video` / `thumb`。UI 历史页直读该文件（解析见 `common/fs_manifest/print_history_manifest.*`）。

Snap 依赖预览流已开（同一 SHM）；与 UI 解码线程共享 ring，偶发 `snap miss` 见日志。

## IPC / SHM

- **Server**：`set_mode_ipc_server(APP_CAMERA)`，`/tmp/upc_camera.sock`
- **预览**：`UI_Camera_Stream_*` / `APP_Camera_Stream_*`

| MsgType | `CameraService` 动作 |
|---------|----------------------|
| `UI_Camera_Stream_Start_Req` | 开采集 + 导出 dma fd + 建 SHM 状态环 + 启动 dma fd 旁路 + 推流线程 |
| `UI_Camera_Stream_Stop_Req` | 停流、关闭 dma fd 旁路、销毁 SHM |
| `APP_Print_Video_Session_Start_Ntf` | 建录像会话、开 dma fd 元数据环 consumer |
| `APP_Print_Layer_Snap_Ntf` | 入队 LayerSnap job |
| `APP_Print_Video_Session_Finish_Ntf` | 入队 Finish，remux `.mp4` + 末帧 `.jpg` + upsert manifest（`complete`） |
| `APP_Print_Video_Session_Abort_Ntf` | 入队 Abort，有帧同样 remux + upsert（`terminated`） |

Glavo→Camera 经 UI Hub 转发；`sender_id` 仍为 `APP_GLAVO`（见 IPC.md）。

可选：`APP_Camera_Frame_Ntf`（`CAMERA_FRAME_NTF_DEFAULT`，UI 预览走元数据环轮询时通常可关）。

## 默认参数

| 项 | 默认 | 环境变量 / 宏 |
|----|------|---------------|
| 设备 | `/dev/video0` | `UPC_CAMERA_DEVICE` |
| 分辨率 | 1024×1280 | `CAMERA_WIDTH/HEIGHT_DEFAULT` |
| 帧率 | 30 | `UPC_CAMERA_FPS` |
| IQ | `/home/wpcam_NC_NC.json` | `UPC_CAMERA_IQ_DIR` / `UPC_CAMERA_IQ_FILE` |
| V4L2 缓冲 | 4 | `CAMERA_V4L2_BUF_COUNT` |
| SHM 槽位 | 3 | `CAMERA_SHM_SLOT_COUNT` |
| 录像临时 | `/tmp/print_vid/{session}/layers.h264` | `CAMERA_PRINT_VIDEO_TMP_DIR` |
| 录像输出 | `/home/video/{job}_{YYYYMMDD_HHMMSS}.mp4` | `CAMERA_PRINT_VIDEO_OUT_DIR` / `_EXT` |
| 缩略图 | `/home/video/{job}_{YYYYMMDD_HHMMSS}.jpg`（抽 MP4 末帧，`-vf scale` 按最大宽度等比缩小） | `CAMERA_PRINT_VIDEO_THUMB_EXT` |
| 缩略图最大宽 | 360 | `CAMERA_PRINT_VIDEO_THUMB_MAX_W` |
| 历史清单 | `/home/video/manifest.json` | `UPC_PRINT_VIDEO_MANIFEST` |
| ffmpeg | `/usr/bin/ffmpeg` | `CAMERA_FFMPEG_BIN` |
| 层编码 fps | 5 | `CAMERA_PRINT_VIDEO_LAYER_FPS` |

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

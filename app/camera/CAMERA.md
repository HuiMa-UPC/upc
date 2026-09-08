# camera/ — 相机后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 采集进程：**`rongsu_camera`**（仅采集/推流，IPC Server）  
> 媒体进程：**`rongsu_media`** — 见 [media/MEDIA.md](../media/MEDIA.md)

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | IPC Server、`CameraService` 生命周期、`process(2)` 循环 |
| `lib/camera_service.*` | 流控 IPC、dma fd 元数据环推流（UI / Visual / Media 多 peer） |
| `lib/vi_capture.*` | rkaiq ISP + V4L2 NV12 采集 |
| `lib/camera_dma_frame_source.*` | V4L2 dma fd 导出、stride padding；开流经主 IPC SCM_RIGHTS 传 fd |
| `lib/device_config.hpp` | 设备、分辨率、IQ、SHM 宏 |

## 数据路径

### 预览

真实相机路径使用 **dma fd 旁路**：

```
Sensor → rkaiq ISP → /dev/video0 (NV12)
  → ViCapture VIDIOC_EXPBUF 导出 dma_buf fd
  → CameraService 推流线程：
      · 经主 IPC（/tmp/upc_camera.sock）SCM_RIGHTS 一次性把所有 fd 发给请求方
      · SHM 元数据环按消费者拆分：
        UI `/upc_camera_preview`、Visual `/upc_camera_preview_visual`、
        Media `/upc_camera_preview_media`
        （只保留 slot 状态/seq/buffer_idx，不再存像素）
  → 消费者：
      · MsgPort::take_peer_dma_fds(APP_CAMERA) 接收 fd 表
      · 按 buffer_idx 找到 dma_buf fd
      · UI：RGA importbuffer_fd(src_fd) NV12→BGRX（零拷贝源）；失败则 mmap 软件回退
      · Media/Visual：mmap 源 fd 读 NV12
```

- 若 V4L2 输出 stride 不满足 RGA 对齐要求，后端把数据拷贝到 `CameraDmaFrameSource` 内部的对齐 dma_buf，再交给 UI。
- UI 不再分配中间 NV12 staging；源 fd 直接来自 V4L2 或后端 padding buffer。
- 状态机：后端 `DQBUF` 后把 `buffer_idx` 写入 `EMPTY` slot 并 `commit` 为 `READY`；消费者 `acquire_latest_frame` 取到 `buffer_idx` 后查 fd 表，用完 `release_slot` 置 `EMPTY`；后端 `wait_slot_empty` 等到 `EMPTY` 才把对应 V4L2 buffer `QBUF` 回去。
- **采集看门狗**：`stream_thread` 若超过 `CAMERA_STREAM_WATCHDOG_STALL_MS`（默认 3s）无成功 dequeue，则原地重启 ISP/V4L2（不清 peer 表），重建各 peer SHM 并重推 `*_Stream_Start_Rsp` + 新 dma fd；两次重启间隔至少 `CAMERA_STREAM_WATCHDOG_COOLDOWN_MS`（默认 8s）。Media 据此重绑 fd，并按 `local_rtsp_desired_` 决定是否重建本地 RTSP。
- 参考 `doc/rv1106` ISP 初始化；当前无 Rockit VENC，不经 MPI 预览。

拍照 / 录像 / 延时 / 直播不在本进程，见 [media/MEDIA.md](../media/MEDIA.md)。

## IPC / SHM

- **Server**：`set_mode_ipc_server(APP_CAMERA)`，`/tmp/upc_camera.sock`
- **预览**：`UI_Camera_Stream_*` / `VISUAL_Camera_Stream_*` / `MEDIA_Camera_Stream_*`

| MsgType | 处理方动作 |
|---------|----------------------|
| `UI_Camera_Stream_Start_Req` | UI 开采集 + 导出 dma fd + 建 UI SHM 状态环 + 启动推流线程 |
| `UI_Camera_Stream_Stop_Req` | UI 停流（仅移除 UI 会话；无其他消费者时停相机） |
| `VISUAL_Camera_Stream_Start_Req` | Visual 开并行会话 + 建 Visual SHM 状态环 |
| `VISUAL_Camera_Stream_Stop_Req` | Visual 停流（仅移除 Visual 会话） |
| `MEDIA_Camera_Stream_Start_Req` | Media 并行取流（独立 SHM `/upc_camera_preview_media`） |
| `MEDIA_Camera_Stream_Stop_Req` | Media 停流（仅移除 Media 会话） |

可选：`APP_Camera_Frame_Ntf`（`CAMERA_FRAME_NTF_DEFAULT`，UI 预览走元数据环轮询时通常可关）。

无其他 peer 时才真正停 ISP/V4L2。

## 默认参数

| 项 | 默认 | 环境变量 / 宏 |
|----|------|---------------|
| 设备 | `/dev/video0` | `UPC_CAMERA_DEVICE` |
| 分辨率 | 1080×1280 | `CAMERA_WIDTH/HEIGHT_DEFAULT` |
| 帧率 | 30 | `UPC_CAMERA_FPS` |
| IQ | `/home/wpcam_NC_NC.json` | `UPC_CAMERA_IQ_DIR` / `UPC_CAMERA_IQ_FILE` |
| V4L2 缓冲 | 4 | `CAMERA_V4L2_BUF_COUNT` |
| SHM 槽位 | 6 | `CAMERA_SHM_SLOT_COUNT`（多消费者；过小易 `slot release timeout`） |
| 看门狗无帧阈值 | 3000 ms | `CAMERA_STREAM_WATCHDOG_STALL_MS`（0=关） |
| 看门狗重启冷却 | 8000 ms | `CAMERA_STREAM_WATCHDOG_COOLDOWN_MS` |

SHM 元数据环：`stride_y` 为 V4L2 实际 Y stride（dma 模式），`buffer_idx` 指向 fd 表；像素走 dma fd。

## 红线

- 禁止链接 `ui::port`、`upc::glavo`、lvgl
- 改消息契约：[common/COMMON.md](../../common/COMMON.md) 四步

## 相关文档

- [media/MEDIA.md](../media/MEDIA.md) · [common/COMMON.md](../../common/COMMON.md) · [ui/UI.md](../ui/UI.md) · [glavo/GLAVO.md](../glavo/GLAVO.md)

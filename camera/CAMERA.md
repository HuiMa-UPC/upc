# camera/ — 相机后端进程

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_camera`**

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | IPC Server、`CameraService` 生命周期、`process(2)` 循环 |
| `lib/camera_service.*` | 流控 IPC、SHM 推流、录像消息分派 |
| `lib/vi_capture.*` | rkaiq ISP + V4L2 NV12 采集 |
| `lib/print_video_recorder.*` | SHM 取帧 → MPP H.264 → `/home/video/*.h264` |
| `lib/device_config.hpp` | 设备、分辨率、IQ、SHM、录像路径宏 |

## 数据路径

### 预览

```
Sensor → rkaiq ISP → /dev/video0 (NV12)
  → CameraService 推流线程 → SHM /upc_camera_preview（3 slot 环）
  → UI CameraPreviewPlayer：解码线程读 SHM → RGA NV12→BGRX（或 SW 回退）→ lv_image
```

- UI 侧：`RgaDmaBuf` 在**开流时**分配 staging + 三帧 BGRX，**关流时**释放；无进程级 RGA 预热。
- SHM 帧可能非 dma32，UI 先 `memcpy` 至 staging 再 RGA（见 `ui/lib/core/rga_dma_buf.*`）。
- 参考 `doc/rv1106` ISP 初始化；当前无 Rockit VENC，不经 MPI 预览。

### 打印录像

```
Glavo Snap IPC → PrintVideoRecorder  worker
  → SHM acquire_latest_frame → MPP encode → /tmp/print_vid/{session}/layers.h264
  → finish/abort → EOS flush → /home/video/{job}.h264
```

Snap 依赖预览流已开（同一 SHM）；与 UI 解码线程共享 ring，偶发 `snap miss` 见日志。

## IPC / SHM

- **Server**：`set_mode_ipc_server(APP_CAMERA)`，`/tmp/upc_camera.sock`
- **预览**：`UI_Camera_Stream_*` / `APP_Camera_Stream_*`

| MsgType | `CameraService` 动作 |
|---------|----------------------|
| `UI_Camera_Stream_Start_Req` | 开采集 + 建 SHM + 推流线程 |
| `UI_Camera_Stream_Stop_Req` | 停流、销毁 SHM |
| `APP_Print_Video_Session_Start_Ntf` | 建录像会话、开 SHM consumer |
| `APP_Print_Layer_Snap_Ntf` | 入队 LayerSnap job |
| `APP_Print_Video_Session_Finish_Ntf` | 入队 Finish，保存 `.h264` |
| `APP_Print_Video_Session_Abort_Ntf` | 入队 Abort，有帧同样保存 |

Glavo→Camera 经 UI Hub 转发；`sender_id` 仍为 `APP_GLAVO`（见 IPC.md）。

可选：`APP_Camera_Frame_Ntf`（`CAMERA_FRAME_NTF_DEFAULT`，UI 预览走 SHM 时可关）。

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
| 录像输出 | `/home/video/{job}.h264` | `CAMERA_PRINT_VIDEO_OUT_DIR` / `_EXT` |
| 层编码 fps | 5 | `CAMERA_PRINT_VIDEO_LAYER_FPS` |

SHM 紧凑 NV12：`stride_y = width`（见 `camera_service` / `ipc_shm_layout`）。

## 红线

- 禁止链接 `ui::port`、`upc::glavo`、lvgl
- 改消息契约：[common/README.md](../common/README.md) 四步

## 相关文档

- [common/ipc/IPC.md](../common/ipc/IPC.md) · [ui/UI.md](../ui/UI.md) · [glavo/GLAVO.md](../glavo/GLAVO.md)

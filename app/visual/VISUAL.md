# visual/ — 视觉识别后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_visual`**

## 职责

- 通过 Camera 的 dma fd + SHM 元数据环跨进程取最新预览帧
- 使用 OpenCV 做 `NV12 -> RGB`，并运行轻量矩形检测 demo
- 保留首层检测请求/应答链路（`GLAVO_Visual_FirstLayer_Check_Req` / `APP_Visual_FirstLayer_Check_Rsp`）
- 打印前光路检测：红点识别桩 `RedDotDetector`，窗口轮询最新 NV12；算法接入见 [doc/视觉协作-光路检测.md](../../doc/视觉协作-光路检测.md)
- 基板测高 / 高度校准：`plate_height_pipeline` 只回光斑质心；LUT 在 Glavo。见 [doc/视觉协作-基板测高.md](../../doc/视觉协作-基板测高.md)

## IPC

- `VISUAL_Camera_Stream_Start_Req`：向 Camera 申请视觉流
- `APP_Visual_Camera_Stream_Start_Rsp`：返回视觉专用 SHM 名称及流参数（并附带 dma fd）
- `VISUAL_Camera_Stream_Stop_Req` / `APP_Visual_Camera_Stream_Stop_Rsp`
- 光路检测会话：`GLAVO_Visual_OpticalPath_Session_Start_Ntf` / `_Stop_Ntf`
- 单点识别：`GLAVO_Visual_OpticalPath_Point_Check_Req` / `APP_Visual_OpticalPath_Point_Check_Rsp`
- 测高会话：`GLAVO_Visual_PlateHeight_Session_Start_Ntf` / `_Stop_Ntf`
- 质心：`GLAVO_Visual_Laser_Spot_Measure_Req` / `APP_Visual_Laser_Spot_Measure_Rsp`

首层、光路与测高共用一路 Camera 取流，会话独立开关。取流生命周期按「引用计数」处理，
判据统一走 `any_session_active_locked()`，**下列三处都必须用它，漏一处就会静默失效**：

| 位置 | 作用 | 漏用后果 |
| --- | --- | --- |
| `send_stream_start_req()` | 任一会话激活就起流 | 光路检测单独激活时不起流 |
| `on_stream_start_rsp()` | 判断这份起流回执是否还有人要 | 把自己请来的 fd 当野包关掉，不 mmap、取帧线程起不来，检测必失败 |
| `release_stream_if_idle()` | 所有会话都停了才拆流 | 一类会话的 Stop 拆掉另一类正在用的流 |

## 数据链路

```text
rongsu_camera
  ├─ 主 IPC SCM_RIGHTS 发送 dma fd 表
  └─ /upc_camera_preview_visual（IpcDmaFrameRing 元数据）
            ↓
rongsu_visual
  ├─ take_peer_dma_fds(APP_CAMERA)
  ├─ consumer.acquire_latest_frame() 拿 buffer_idx
  ├─ mmap(fd) + OpenCV cvtColor(NV12->RGB)
  └─ 矩形检测（当前不再发布独立四点坐标消息）
```

## 说明

- 视觉 demo 的检测目标为最大凸四边形（轮廓近似）。
- 若当前工具链没有 OpenCV，会编译通过但不会启用识别路径（仅推送 `valid=0`）。

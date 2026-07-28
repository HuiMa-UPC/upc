# visual/ — 视觉识别后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_visual`**

## 职责

- 通过 Camera 的 dma fd + SHM 元数据环跨进程取最新预览帧
- 使用 OpenCV 做 `NV12 -> RGB`，并运行轻量矩形检测 demo
- 将检测到的四点坐标通过 `APP_Visual_Rect_Ntf` 推送给 UI

## IPC

- `VISUAL_Camera_Stream_Start_Req`：向 Camera 申请视觉流
- `APP_Visual_Camera_Stream_Start_Rsp`：返回视觉专用 SHM 名称及流参数（并附带 dma fd）
- `VISUAL_Camera_Stream_Stop_Req` / `APP_Visual_Camera_Stream_Stop_Done`
- `APP_Visual_Rect_Ntf`：视觉识别结果（四点坐标）

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
  └─ 矩形检测 -> APP_Visual_Rect_Ntf -> UI 叠加画框
```

## 说明

- 视觉 demo 的检测目标为最大凸四边形（轮廓近似）。
- 若当前工具链没有 OpenCV，会编译通过但不会启用识别路径（仅推送 `valid=0`）。

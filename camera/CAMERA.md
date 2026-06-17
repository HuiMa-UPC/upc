# camera/ — 相机后端进程

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_camera`**

## 职责

| 组件 | 路径 | 说明 |
|------|------|------|
| `CameraService` | `lib/camera_service.*` | IPC + SHM 推流 |
| `ViCapture` | `lib/vi_capture.*` | rkaiq ISP + V4L2 NV12 采集 |
| `device_config.hpp` | `lib/device_config.hpp` | 设备、分辨率、IQ、SHM |

## 采集路径

参考 `doc/rv1106` 的 ISP 初始化逻辑；当前工具链无 Rockit MPI，故 **不经过 VENC**，直接：

```
Sensor → rkaiq ISP → /dev/video0 (V4L2 NV12) → SHM → UI RGA 预览
```

## IPC / SHM

- **Server**：`set_mode_ipc_server(APP_CAMERA)`，默认 `/tmp/upc_camera.sock`
- **消息**：`UI_Camera_Stream_*` / `APP_Camera_Stream_*`
- **像素**：NV12 帧环 `/upc_camera_preview`（与 `test_camera` 相同契约）

## 默认参数

| 项 | 默认 | 环境变量 |
|----|------|----------|
| 设备 | `/dev/video0` | `UPC_CAMERA_DEVICE` |
| 分辨率 | 1024×1280 | — |
| 帧率 | 30 | `UPC_CAMERA_FPS` |
| IQ 目录 | `/home` | `UPC_CAMERA_IQ_DIR` |
| IQ 文件 | `wpcam_NC_NC.json` | `UPC_CAMERA_IQ_FILE` |

IQ 目录下需有主 json 及 rkaiq 依赖的配套文件（与 rv1106 相同）。

## 红线

- 禁止链接 `ui::port`、`upc::glavo`、lvgl
- 改消息契约走 [contract/README.md](../common/contract/README.md) 四步

## 相关文档

- [common/ipc/IPC.md](../common/ipc/IPC.md) · [ui/UI.md](../ui/UI.md) · [doc/rv1106](../doc/rv1106)

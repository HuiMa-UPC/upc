# test/ — 独立测试进程

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)

与 `app/`、`test/` 同级，用于板端/联调桩，**不链接** LVGL 与 `app/glavo/lib`。

## 当前进程

| 可执行文件 | 目录 | 说明 |
|------------|------|------|
| `test_camera` | `test/camera/` | 摄像头预览 dma fd 生产者；IPC Server |

## test_camera

- **入口**：`test/camera/main.cpp`
- **IPC**：`set_mode_ipc_server(APP_CAMERA)`，默认 `/tmp/upc_camera.sock`
- **SHM**：`/upc_camera_preview`（dma fd 元数据环，见 `ipc_dma_frame_ring`）
- **数据源**：`file_frame_source` 读 `.nv12` 等（配置见 `camera_test_config.hpp`）
- **日志**：`/var/log/test_camera.log`，`UPC_MOD_TEST_CAMERA`

### 与 UI 关系

```
test_camera ──dma fd + 元数据环──► CameraPreviewPlayer（rongsu_ui）
            ──IPC───────────────► CameraMsgProxy（Start/Stop stream）
                                    └─ RGA/SW NV12→BGRX + lv_image（与 rongsu_camera 同契约）
```

UI 解码路径与对接 `rongsu_camera` 相同；差异仅在数据源（文件帧 vs 真机 V4L2），两者都走 dma fd 旁路。

`scripts/rongsu.sh`：`UPC_ENABLE_TEST_CAMERA=auto` 时若存在二进制则启动。

## 红线

- 禁止链接 `ui::port`、`upc_glavo`
- 消息类型与 UI 约定见 `msg_types.hpp` + [common/README.md](../common/README.md)

## 相关文档

- [app/ui/UI.md](../app/ui/UI.md) · [common/ipc/IPC.md](../common/ipc/IPC.md)

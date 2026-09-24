# visual/ — 视觉识别后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_visual`**

## 职责

- 通过 Camera 的 dma fd + SHM 元数据环跨进程取最新预览帧
- 使用 OpenCV 做 `NV12 -> RGB`，并运行轻量矩形检测 demo
- 保留首层检测请求/应答链路（`GLAVO_Visual_FirstLayer_Check_Req` / `APP_Visual_FirstLayer_Check_Rsp`）
- 打印前光路检测：红点识别桩 `RedDotDetector`，窗口轮询最新 NV12；算法接入见 [doc/视觉协作-光路检测.md](../../doc/视觉协作-光路检测.md)
- 内门挡镜头 / 平台码标记检测：已补算法及 Visual IPC 接收侧，SOC 调用侧未接入、协议待双方审查。当前接入与测试以 [CABIN_IPC_INTEGRATION.md](../../doc/CABIN_IPC_INTEGRATION.md) 为准；门、Z、起印和 UI 改动不在本次交付范围。

## IPC

- `VISUAL_Camera_Stream_Start_Req`：向 Camera 申请视觉流
- `APP_Visual_Camera_Stream_Start_Rsp`：返回视觉专用 SHM 名称及流参数（并附带 dma fd）
- `VISUAL_Camera_Stream_Stop_Req` / `APP_Visual_Camera_Stream_Stop_Rsp`
- 光路检测会话：`GLAVO_Visual_OpticalPath_Session_Start_Ntf` / `_Stop_Ntf`
- 单点识别：`GLAVO_Visual_OpticalPath_Point_Check_Req` / `APP_Visual_OpticalPath_Point_Check_Rsp`

- 舱检会话：`GLAVO_Visual_Cabin_Session_Start_Ntf` / `_Stop_Ntf`
- 内门：`GLAVO_Visual_InnerDoor_Occlusion_Req` / `APP_Visual_InnerDoor_Occlusion_Rsp`
- 基板：`GLAVO_Visual_Plate_Present_Req` / `APP_Visual_Plate_Present_Rsp`

首层、光路及舱检共用一路 Camera 取流。舱检与前两者互斥，已有功能优先，
舱检自身的门检测与基板检测也禁止并行。不影响已有功能之间的会话关系。
舱检得出一轮结果后停止检测并释放取流需求，等下一条显式请求，不在放板过程中自动再检。
取流生命周期按「引用计数」处理，
判据统一走 `any_session_active_locked()`，**下列三处都必须用它，漏一处就会静默失效**：

| 位置 | 作用 | 漏用后果 |
| --- | --- | --- |
| `send_stream_start_req()` | 任一会话激活就起流 | 光路检测单独激活时不起流 |
| `on_stream_start_rsp()` | 判断这份起流回执是否还有人要 | 把自己请来的 fd 当野包关掉，不 mmap、取帧线程起不来，检测必失败 |
| `release_stream_if_idle()` | 两类会话都停了才拆流 | 一类会话的 Stop 拆掉另一类正在用的流 |

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

- 舱检算法是 `lib/cabin_detector.*`，仅接紧凑 NV12 并返回单帧结果；`lib/visual_cabin_service.cpp` 管理请求窗口、新帧连续证据、超时和回包。
- 不使用 RTSP 或 UI 预览作为产品算法输入，不控制机构、舱灯、Z 或打印。
- 两类算法均须加载有效标定，缺少或格式错误返回 unknown。部署路径及候选样本限制见 [calibration/README.md](calibration/README.md)。
- 隔离测试见 [tests/README.md](tests/README.md)，使用独立 Broker 与模拟 Camera，不连接运行中的设备服务。

- 视觉 demo 的检测目标为最大凸四边形（轮廓近似）。
- 若当前工具链没有 OpenCV，会编译通过但不会启用识别路径（仅推送 `valid=0`）。

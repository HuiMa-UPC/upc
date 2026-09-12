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
- 状态机：后端 `DQBUF` 后把 `buffer_idx` 写入 `EMPTY` slot 并 `commit` 为 `READY`；消费者 `acquire_latest_frame` 取到 `buffer_idx` 后查 fd 表，用完 `release_slot` 置 `EMPTY`；buffer 要等所有 peer 都归还槽位后才 `QBUF` 回去。
- **`dequeue_frame` 排空到最新帧**：V4L2 是 FIFO，一次 `DQBUF` 拿到的是队列里最老的一帧。采集线程按固定帧周期"取一还一"，而 `sleep_for` 的过冲让它比 sensor 慢约 0.1ms/帧——这点差额只能累积、永远排不掉，实测 40 秒内把画面延迟从 20ms 顶到 141ms 并让驱动开始丢帧。所以首帧之后要继续非阻塞 `DQBUF` 直到 `EAGAIN`，把过期帧立即 `QBUF` 还回去，只交付最新一帧（fd 以 `O_NONBLOCK` 打开，队列空时立即返回，不会阻塞）。
  代价是消费者卡顿期间过期帧被丢弃而不是排队，**打印录像在卡顿瞬间会少几帧**；稳态 30fps 进 30fps 出时每轮通常一帧都排不出来，可忽略。排掉的帧数记在 `drv_drained`。
- **在飞窗口（不锁步）**：采集线程 commit 后不逐帧等待，而是把该帧挂进在飞队列，每轮用 `slot_is_empty` 非阻塞回收已完成的帧；只有队列达到 `CAMERA_MAX_FRAMES_IN_FLIGHT` 才 `wait_slot_empty` 等最老一帧（超时仍强制归还 buffer，宁可让慢消费者读到被覆写的画面，也不停整条流）。
  这样单个消费者的瞬时抖动（UI 交互降频、Visual 分割、Media 编码/推流）被窗口吸收，不会把采集帧率拉到最慢消费者的速率。窗口取 `min(CAMERA_MAX_FRAMES_IN_FLIGHT, buffer 数-2, 槽位数-1)`。
- **等待上限按帧率推导，不能写死**：`wait_slot_empty` 的超时由 `peer_wait_slot_empty_ms(stream_fps_)` 算出 `kPeerWaitSlotEmptyFramePeriods`（2）个帧周期，@30fps 约 66ms。这个等待只为「让消费者用完当前帧再复用 buffer」争取时间，超时后照样归还，所以上限只需覆盖消费者处理一帧的正常耗时（UI 侧实测单帧 RGA 转换约 7ms）。
  **曾写死 1000ms，是一个真实故障的根因**：任一消费者卡住，采集线程就每帧白等整整 1 秒，把整条采集链路（含直播编码与打印逐层快照）一起拖到 1fps 且不自行恢复——现场复现出连续 48 分钟、间隔精确 1.037 秒（1000ms 超时 + 约 37ms 真实工作）的预览塌陷，`slot release timeout` WARN 同频刷屏。按帧率推导后，最坏退化被限制成「该消费者掉几帧」，不再扩散到其它消费者。
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
| V4L2 缓冲 | 4 | `CAMERA_V4L2_BUF_COUNT`（**上调前先读该宏的注释**：它同时约束驱动队列深度、padding pool 大小、以及 UI 侧 RGA 常驻导入数；实测 6 会让 UI 的 RGA 导入失败、静默退化成 22ms/帧的软件转换） |
| 在飞帧上限 | 3 | `CAMERA_MAX_FRAMES_IN_FLIGHT`（实际取 `min(本值, 缓冲数-2, 槽位数-1)`，缓冲数 4 时是 2、@30fps 约吸收 66ms 抖动） |
| SHM 槽位 | 6 | `CAMERA_SHM_SLOT_COUNT`（多消费者；过小易 `slot release timeout`） |
| 槽位归还等待 | 2 个帧周期（@30fps ≈ 66ms） | `kPeerWaitSlotEmptyFramePeriods`（下限 `kPeerWaitSlotEmptyMinMs` = 20ms；**不要改回固定 1000ms**，理由见上文在飞窗口一节） |
| 看门狗无帧阈值 | 3000 ms | `CAMERA_STREAM_WATCHDOG_STALL_MS`（0=关） |
| 看门狗重启冷却 | 8000 ms | `CAMERA_STREAM_WATCHDOG_COOLDOWN_MS` |

SHM 元数据环：`stride_y` 为 V4L2 实际 Y stride（dma 模式），`buffer_idx` 指向 fd 表；像素走 dma fd。

## 延迟诊断

`stream 5s:` 一行（`/var/log/topic/topic_ui_preview.log`，`proc=rongsu_camera`）除流量计数外还带取帧延迟字段，用于定位"预览延迟大"落在哪一段：

| 字段 | 含义 | 正常范围 |
|------|------|----------|
| `drv_age_avg_us` / `drv_age_max_us` | 交付帧在驱动队列里躺了多久（`buf.timestamp` 与单调时钟之差） | @30fps 约 ≤ 33000 且**不随时间爬升**；持续增长说明排空没生效或另有阻塞点 |
| `drv_drained` | 排空丢弃的过期帧数 | 稳态接近 0；持续 >0 说明采集线程跟不上 sensor，但延迟已被排空兜住 |
| `drv_drop` | `buf.sequence` 跳号累计，即驱动因无空闲 buffer 丢掉的帧数 | 0；持续增长说明缓冲不足或采集线程被拖住 |
| `ok`（`decode 5s:` 行） | UI 侧实际取到的帧数 | 应与本窗口 `committed` 相当（@30fps 约 150）；若稳定是 `committed` 的一半，怀疑元数据环 seq 语义（见 `common/COMMON.md` § 预览元数据环 seq 契约） |
| `ts_mono` | 驱动时间戳是否为 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` | 1；为 0 时 `drv_age_*` 无意义（恒为 0），需换手段判断积压 |
| `inflight_max` | 本窗口在飞队列峰值 / 窗口上限 | 常态 1；贴到上限说明有消费者持槽过久 |

`drv_age_*` 会不会随时间单调爬升，是判断排空是否生效的关键——排空前实测 40 秒从 20ms 涨到 141ms，排空后应当稳定。`drv_drained` 与 `drv_drop` 的区别：前者是我们主动丢的过期帧（延迟换新鲜度，正常），后者是驱动因为没有空闲 buffer 被迫丢的（异常）。

因此 `sequence` 跟踪（`note_dequeued_sequence`）必须对**每一帧 `DQBUF`** 调用，含被排空丢弃的帧；只对交付帧调用的话，我们主动丢掉的帧会在帧号上留下空缺被记进 `drv_drop`，读数恒等于 `drv_drained`，两个字段就都失去了区分能力。滞留时间（`accumulate_delivered_latency`）反过来只对交付帧统计——被丢的帧不上屏，不构成画面延迟。

## 红线

- 禁止链接 `ui::port`、`upc::glavo`、lvgl
- 改消息契约：[common/COMMON.md](../../common/COMMON.md) 四步

## 相关文档

- [media/MEDIA.md](../media/MEDIA.md) · [common/COMMON.md](../../common/COMMON.md) · [ui/UI.md](../ui/UI.md) · [glavo/GLAVO.md](../glavo/GLAVO.md)

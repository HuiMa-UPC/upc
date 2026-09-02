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
| `../media/lib/print_video_recorder.*` | 元数据环取帧 → ffmpeg 软编 PNG → `/home/media/video/tmp/` + `.rthm` |
| `../media/lib/print_h264_recorder.*` | 预览 NV12 异步入队；worker RKMPP 实时编 `print.h264`（队列满丢帧） |
| `../media/lib/clean_video_recorder.*` | 清粉延时抽帧：按 `TIMELAPSE_VIDEO_FPS` 写接续 PNG，成功后重建 `timelapse.mp4`+`thumb.rthm` 并删除清粉 PNG |
| `../media/lib/h264_stream_encoder.*` | 直播共用 RKMPP 编码器：NV12 单槽 latest-wins → Annex-B → 多 sink 分发（带采帧 pts）；缓存 SPS/PPS |
| `../media/lib/live_rtsp_pusher.*` | 云端 live：编码器 sink → 发送队列 → writer 线程 → ffmpeg `-c copy -f rtsp`（`push_url` 原样；不摘 Camera peer） |
| `../media/lib/local_rtsp_server.*` | 局域网 live：编码器 sink → live555 RTSP Server（上电默认开，调试开关可改） |
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
- **采集看门狗**：`stream_thread` 若超过 `CAMERA_STREAM_WATCHDOG_STALL_MS`（默认 3s）无成功 dequeue，则原地重启 ISP/V4L2（不清 peer 表），重建各 peer SHM 并重推 `*_Stream_Start_Rsp` + 新 dma fd；两次重启间隔至少 `CAMERA_STREAM_WATCHDOG_COOLDOWN_MS`（默认 8s）。
- `PrintVideoRecorder` 也改为通过 fd + mmap 读取最新帧。
- 参考 `doc/rv1106` ISP 初始化；当前无 Rockit VENC，不经 MPI 预览。

### 媒体落盘根目录

```
/home/media/
  photo/          相册 PNG + .rthm
  video/
    config.json   打印历史清单（延时 + 可选打印录像）
    tmp/          唯一打印中临时目录（开打前/开机清空）
    {job}/        正式会话产物
```

旧路径 `/home/photo`、`/home/video`（及 `manifest.json`）在 Media 启动时自动迁入（若新路径尚不存在）。
遗留 `{job}.__work_*` 也会在开机/开打前清理。

存储水位（`statvfs(/home/media)`）：
- ≥90%：本会话停 H264、跳过层间拍照
- ≥95%：强制清空 `tmp/` 与遗留 `__work_*`

### 打印录像

```
Glavo Snap IPC → PrintVideoRecorder worker
  → `IpcDmaFrameRingConsumer::acquire_latest_frame` 拿到 `buffer_idx`
  → 通过 fd 回调 `mmap` 取 NV12（如 stride 有 padding 会按 active width 解压）
  → ffmpeg 软编 PNG（连续编号 1.png,2.png,...）
      → /home/media/video/tmp/
  → finish/abort
  → 缩略图：dma32 + RGA importbuffer_fd 生成 thumb.rthm（失败则无缩略图）
  → upsert /home/media/video/config.json（frames_dir=tmp，status=complete|terminated）
  → APP_Print_Video_Manifest_Ntf → UI（切打印结果页）
  → 结果页退出 finalize：tmp → {job}/；延时默认 PNG 播放（`UPC_TIMELAPSE_PLAYBACK_PNG=1` 不编 timelapse.mp4）；开启打印录像时仍 remux print_h264.mp4（PTS 用相机流帧率，默认 30fps，勿用 layer_fps）
```

0 帧时不生成层间产物、不写清单，但**仍会发** `APP_Print_Video_Manifest_Ntf`
（`has_entry=0` + `aborted` 标志），否则 UI 会永远停在打印中界面下不来。
此时仅删除**空**的 `tmp/`，以免误删同目录下正在写入的 `print.h264`。

### 打印连续 H264（调试开关）

```
预览帧 → 推流线程：拷贝 NV12 入队（队列满丢帧，不 fwrite）
  → worker：顺序 append print_raw.nv12
  → Finish/Abort：active=0 + 入队 Encode（消息线程立刻返回）
  → worker：排空写帧后 mpi_enc → print.h264，成功后删 raw
```

与层间 `PrintVideoRecorder` worker 并行；结果页仍只等层间 Manifest，不依赖 H264 编完。
磁盘跟不上时录像可能丢帧（日志 `print_h264 queue full drop=...`），优先保证预览不卡死。
CBR 码率默认 `CAMERA_PRINT_H264_BITRATE`=2048kbps（与直播共用同一常量）；可由 `cal.csv` Config 段 `PRINT_H264_BITRATE`（500000–8000000）覆盖，经 `PrintVideoSessionPayload.h264_bitrate` 下发。

### 直播（云端推流 + 局域网 RTSP Server）

两路直播**共用同一路 H.264 编码**（`H264StreamEncoder`），只编一次、分发多份：

```
预览帧 → H264StreamEncoder.on_frame_nv12（单槽 latest-wins，积压丢旧帧；此处打采帧时间戳）
  → worker：RKMPP CBR 编 Annex-B（GOP=fps/2≈0.5s，每个 IDR 都带 SPS/PPS）
  → sink 分发（带同一帧的 pts_us）：
      · LiveRtspPusher   → 入发送队列 → writer 线程 write(ffmpeg stdin)
                         → -c copy -f rtsp → 云端 push_url
      · LocalRtspServer  → 拆裸 NAL 入队 + triggerEvent
                         → live555 事件循环：H264VideoStreamDiscreteFramer → RTP
```

- 码率**固定**取 `CAMERA_PRINT_H264_BITRATE`，不按 sink 指定：改码率需重开 MPP，会打断在途的
  另一路。云端下发的 `max_bitrate_kbps` 不再参与配置，仅在与板端值不同时记一条日志备查。
- **时间戳在采帧时打，一帧一个值，透传给该帧产生的所有包**。不可由各 sink 自行取时刻：
  MPP 可能把一帧拆成多个包（EACH_IDR 下参数集常自成一包），若各包 presentation time 不同，
  live555 会误判 AU 边界、marker bit 置错，客户端报 `missing picture in access unit`；
  取编码完成时刻也不行——编码耗时抖动会灌进 pts，播放器为抹平抖动而加大缓冲、拉高延迟。
- sink 之间互不连坐：某一路投递失败只摘该路；只有 MPP 自身失败才停整个编码会话。
- **sink 回调不得阻塞**：编码器 worker 串行调用各 sink，任一路阻塞都会连带冻结其余路。
  故云端那路只入队即返回，实际 `write()` 下沉到自己的 writer 线程（管道设 `O_NONBLOCK`，
  写满时 `poll(POLLOUT)` 等待）；队列积压超 `UPC_LIVE_PUSH_QUEUE_MAX_BYTES`（默认 1MB）时
  **整段丢弃到下一个 IDR**，而不是丢单个包——给 ffmpeg 送残缺 NAL 会直接破坏码流。
- 会话拆除顺序固定为 **摘 sink → 停 writer → 关 ffmpeg fd**：摘 sink 返回后才没有新包入队，
  而 writer 可能正卡在该 fd 的 `poll` 上，先关 fd 会让它 poll 到已回收的描述符。
- **live555 事件循环随 Media 进程常驻**，不随开关销毁（反复销毁 env/scheduler 易踩回收陷阱）；
  开关只控制 `ServerMediaSession` 的挂载与编码器 sink 的增删。
- 除 `triggerEvent()` 外，所有 live555 API 只能在事件循环线程内调用；挂/摘 session 通过命令队列投递。
- 拉流地址 `rtsp://<本机IP>:8554/live`（一期**不做鉴权**，仅打日志、不在 UI 展示，需手动输入 IP）。
- **强制 RTP-over-TCP**：WiFi 下 UDP 丢包会直接花屏并报 `missed packets`，局域网内交织传输
  的开销可忽略。live555 只有反向开关 `disableStreamingRTPOverTCP()`，故由 `TcpOnlyRtspServer`
  在 SETUP 阶段判定 Transport 头，非 `RTP/AVP/TCP` 一律回 **461 Unsupported Transport**
  并记一条 `local_rtsp SETUP over UDP rejected` 日志。461 是标准的传输协商失败码，
  ffmpeg / VLC 收到后会自动改用 TCP 重发 SETUP，故客户端**不写** `-rtsp_transport tcp`
  也能连上，只是多一轮握手。
  实现上借 `ConnAccessor` 做派生类型转换取得 `handleCmd_unsupportedTransport()` 的访问权——
  该函数只对 `RTSPClientSession` 开放友元，而友元关系不随继承传递。
- 选址（`LocalRtspServer::lan_ipv4_list()`）跳过 loopback、DOWN、虚拟网卡（`dummy`/`docker`/`veth`/…）
  与 `169.254.0.0/16` 链路本地地址，按 `UPC_WIFI_IFACE`(默认 `wlan0`) → 其它无线 → 有线 → USB 排序。
  **不能简单取"第一个非 loopback 接口"**：多网口设备上 `dummy0` 带着 169.254 地址且序号排在真实网卡之前。
  服务本身监听 `0.0.0.0`，日志里 `candidates=[...]` 会列出全部可选地址备查。
- **`OutPacketBuffer::maxSize` 必须调大**（`UPC_LOCAL_RTSP_MAX_FRAME_BYTES`，默认 1MB）：
  live555 默认只有 60000 字节，而 1080×1280 的 I 帧可达 150～200KB，超出部分被 RTPSink
  静默丢弃，客户端每个 I 帧花屏且误差沿 P 帧传播。必须在任何 RTPSink 构造前设好。
- **上电默认开启**（`UPC_LOCAL_RTSP_AUTO_START`，默认 1）。实际拉起时机是相机流就绪之后
  （`on_stream_start_rsp` 末尾）——开流前编码几何未知，`start()` 必失败。
  Media 侧另记 `local_rtsp_desired_`：调试页开关会改写它，相机看门狗重启后据此重建用户
  真正想要的状态，而不是一律回到上电默认值；已在运行则跳过，避免打断在连客户端。
- 相机流未就绪时 Media 回 `on=0`，UI 开关自动弹回。
- 日志：`live_rtsp` / `local_rtsp` / `h264_enc` 三个前缀都归入 `topic_media_rtsp.log`；
  `local_rtsp client stream started/ended` 标记首个客户端接入与最后一个客户端离开；
  live555 库内诊断经 `LogUsageEnvironment` 转成 `local_rtsp live555: …`（默认只落 stderr，不接就看不见）。
- **运行期心跳**：三处各 5s 一行，是排查"画面卡住"的第一现场（此前所有计数只在 stop 时才打印，
  故障时完全看不出断在哪一环）。由 `UPC_MEDIA_LOG_STREAM_STAT`（`upc_contract.hpp`，**默认 0**，
  平时嫌吵，排查时置 1 重编）控制；置 0 时整段代码不编入二进制，计数仍照常累加、
  停流汇总日志不受影响：

  | 日志 | 关键字段 | 怎么读 |
  |---|---|---|
  | `h264_enc stat` | `sinks` / `in` / `enc` / `kbps` / `overwritten` | `in` 不涨=相机没送帧；`in` 涨而 `enc` 不涨=卡在 MPP 或某路 sink 回调；`overwritten`=单槽被新帧覆盖的次数 |
  | `live_rtsp stat` | `in` / `out` / `kbps` / `queue` / `dropped` / `drop_events` | `queue` 持续贴上限=云端吃不下该码率；`in` 涨而 `out` 不涨=写端卡住 |
  | `local_rtsp stat` | `in` / `out` / `queue` / `dropped` / `client` | `queue` 持续贴上限=live555 取不赢，延迟会顶到队列容量对应时长 |

`config.json` 由 Media 独写；字段含 `job_name` / `start_unix` / `duration_sec` / `material_g` / `status` / `frames_dir` / `thumb`。UI 历史页直读该文件（解析见 `common/fs_manifest/print_history_manifest.*`）。

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
| `UI_Camera_Snapshot_Req` | **Media** 保存当前预览帧 PNG + 配对 `.rthm` 缩略图到 `/home/media/photo` |
| `APP_Print_Video_Session_*` | **Media** 处理打印录像会话、生成缩略图/MP4/manifest |
| `APP_Clean_Video_Session_*` | **Media** 处理清粉延时抽帧（接续 PNG → 合并 `timelapse.mp4`） |
| `APP_Print_Video_Manifest_Ntf` | **Media→UI**：manifest `rename()` 成功后发（0 帧也必发） |
| `MQTT_Media_Rtsp_Start/Stop_Req` | **Media** 起停云端 live 推流，`APP_Mqtt_Rtsp_*_Rsp` 回 ack |
| `UI_Debug_Local_Rtsp_Set/Get_Ntf` | **Media** 起停/查询局域网 RTSP Server（调试页开关，直发 `APP_MEDIA`） |
| `APP_Media_Local_Rtsp_State_Ntf` | **Media→UI**：本地 RTSP 实际开关状态（失败回 0，UI 据此弹回） |

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
| 看门狗无帧阈值 | 3000 ms | `CAMERA_STREAM_WATCHDOG_STALL_MS`（0=关） |
| 看门狗重启冷却 | 8000 ms | `CAMERA_STREAM_WATCHDOG_COOLDOWN_MS` |
| 录像输出 | `/home/media/video/tmp/`（打印中）→ finalize 后 `/home/media/video/{job}/` | `UPC_VIDEO_TMP` / `CAMERA_PRINT_VIDEO_OUT_DIR` |
| 缩略图 | `{dir}/thumb.rthm` | `CAMERA_PRINT_VIDEO_THUMB_EXT` |
| 缩略图最大宽 | 240 | `CAMERA_PRINT_VIDEO_THUMB_MAX_W` |
| 拍照原图 | `/home/media/photo/photo_{stamp}_{WxH}.png` | Media `save_latest_png_snapshot` |
| 拍照缩略图 | `/home/media/photo/photo_{stamp}_{WxH}.rthm`（同帧 NV12→dma32+RGA） | 同上；失败不影响 PNG |
| 历史清单 | `/home/media/video/config.json` | `UPC_VIDEO_CONFIG` |
| ffmpeg | `/usr/bin/ffmpeg` | `CAMERA_FFMPEG_BIN` |
| 本地 RTSP 端口 | 8554 | `UPC_LOCAL_RTSP_PORT` / `UPC_LOCAL_RTSP_PORT_DEFAULT` |
| 本地 RTSP 流名 | `live` | `UPC_LOCAL_RTSP_STREAM_NAME` |
| 本地 RTSP 上电默认 | 开（相机流就绪后拉起） | `UPC_LOCAL_RTSP_AUTO_START` |
| 本地 RTSP NAL 队列 | 128（满丢最旧） | `UPC_LOCAL_RTSP_QUEUE_MAX`（30fps 下约合 4s，即积压时的延迟上限） |
| 本地 RTSP 单帧上限 | 1 MB | `UPC_LOCAL_RTSP_MAX_FRAME_BYTES`（`OutPacketBuffer::maxSize`） |
| 云端推流发送队列 | 1 MB（满则丢到下个 IDR） | `UPC_LIVE_PUSH_QUEUE_MAX_BYTES` |
| 直播/录像码率 | 2048 kbps | `CAMERA_PRINT_H264_BITRATE`（直播固定；录像可被 `cal.csv` 覆盖） |
| 直播 GOP | `fps/2`≈0.5s（每秒 2 个 IDR） | `h264_stream_encoder.cpp` `rc:gop` |
| 心跳日志周期 | 5 s | 三处 `kStatsPeriodUs`；开关 `UPC_MEDIA_LOG_STREAM_STAT`（默认 0） |

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

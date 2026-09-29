# media/ — 媒体后端进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_media`**（IPC 客户端，连 Broker）  
> 采集进程见 [camera/CAMERA.md](../camera/CAMERA.md)

`MediaService` 消费 Camera 的 dma fd + SHM 元数据环（`/upc_camera_preview_media`），负责拍照、打印层拍/延时、打印期 H264、清粉抽帧、云端 RTSP 推流与局域网 RTSP Server。

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 进程守护、IPC 客户端、`MediaService` 生命周期、`process(2)` + `tick()` |
| `lib/media_service.*` | 订阅分派、DMA 消费线程、NV12 快照、存储预算、finalize 栅栏、直播开关 |
| `lib/print_video_recorder.*` | 层 Snap→CPU JPEG → `tmp/`；Finish 写 thumb + manifest 快路径；开门 finalize |
| `lib/print_h264_recorder.*` | 预览 NV12 异步入队；worker RKMPP 边编边封装 `print_h264.mp4`（队列满丢帧） |
| `lib/clean_video_recorder.*` | 清粉延时抽帧：按 timelapse fps 接续写 JPEG；MP4 等开门 finalize |
| `lib/timelapse_util.*` | NV12→JPEG、JPEG→MP4、H264 remux、RTHM 缩略图、JPEG 扫描/清理 |
| `lib/h264_stream_encoder.*` | 直播共用 RKMPP：NV12 单槽 latest-wins → Annex-B → 多 sink（采帧 pts）；缓存 SPS/PPS |
| `lib/live_rtsp_pusher.*` | 云端 live：sink 入队 → writer → ffmpeg `-c copy -f rtsp`（`push_url` 原样；不摘 Camera peer） |
| `lib/local_rtsp_server.*` | 局域网 live：sink → live555 RTSP Server（上电默认开，调试开关可改） |
| `lib/device_config.hpp` | 几何初值、缩略图/ffmpeg、H264 码率、延时 fps、水位宏 |

可选依赖（CMake 探测不到则对应路径空实现/关闭）：`librga`、`RKMPP`、`live555`。静图 JPEG 的 `libturbojpeg` 是必选，缺失则配置失败。

## 初始化

1. `set_mode_ipc_client(APP_MEDIA)`
2. 建 `/userdata/media/{photo,video}`（**不再**迁 `/home/photo`、`/home/video`）
3. `recover_pending_and_reset_tmp()`：尽力收尾 pending，再清空 `tmp/` 与遗留 `{job}.__work_*`
4. 注入 snapshot / 存储预算 / H264 finalize 回调；`live_rtsp`/`local_rtsp` 绑同一 `H264StreamEncoder`
5. `local_rtsp.init(port)`：live555 事件循环随进程常驻；**必须绑上 IPv4**，失败则后续 `tick()` 重试
6. 延时 fps 固定 `CAMERA_PRINT_VIDEO_LAYER_FPS`（5），不读 `cal.csv`
7. 立即 `MEDIA_Camera_Stream_Start_Req`；流未就绪则 `tick()` 每 1s 重试

## 取流

```
rongsu_camera
  ├─ 主 IPC SCM_RIGHTS 发 dma fd 表
  └─ /upc_camera_preview_media（IpcDmaFrameRing）
            ↓
rongsu_media worker
  ├─ take_peer_dma_fds(APP_CAMERA) + mmap
  ├─ acquire_latest_frame → buffer_idx
  ├─ 一次紧凑拷贝 dma → staging_nv12_
  ├─ release_slot（持槽期间只做上面这一次拷贝）
  ├─ 仅活跃消费者从 staging 喂 NV12：clean_video / print_h264 / h264_encoder
  │    （未录像、未直播时跳过各自二次全帧拷贝）
  └─ staging 与 latest_nv12_snapshot_ 互换（层拍/拍照用，零拷贝发布）
```

**槽位要尽早归还**：活跃 recorder 各自还要把帧拷进自己的队列，若留在槽内做，持槽时间翻几倍，会占满 Camera 的在飞窗口并反压采集线程（见 [camera/CAMERA.md](../camera/CAMERA.md) 「在飞窗口」）。快照发布用 `swap` 而非再拷一份：staging 下一帧整体覆写，换回来的旧缓冲容量已够。

几何以 `APP_Media_Camera_Stream_Start_Rsp` 为准。流就绪后才 `apply_local_rtsp_desired()`（开流前编码几何未知，`start()` 必失败）。相机看门狗重启会再走 `on_stream_start_rsp`；本地 RTSP 已在跑则跳过，避免打断在连客户端。

## 落盘根目录

路径宏：`common/fs_manifest/media_paths.hpp`（`UPC_MEDIA_ROOT` 默认 `/userdata/media`）。

```
/userdata/media/
  photo/          相册 JPEG + 配对 .rthm
  video/
    config.json   打印历史清单（Media 独写；UI 直读）
    tmp/          唯一打印中临时目录（开机/开打前清空）
    {job}/        正式会话产物
```

`config.json` 字段：`job_name` / `start_unix` / `duration_sec` / `material_g` / `layer_count` / `status` / `frames_dir` / `thumb`。`material_g` 与 `layer_count` 在 upsert 时按 job 基名从 `/userdata/Gcode/config.json` 快照（`print_history_lookup_gcode_stats`），查不到写 0。解析见 `common/fs_manifest/print_history_manifest.*`。

**`job_name` 存原始 G-code 基名，不要净化。** 它同时是回查 `/userdata/Gcode/config.json` 的主键，而那边存的是真实文件名；一旦按 `sanitize_filename()` 把 `~`、`(`、空格等换成 `_`，形如 `xfk-8.10-0.03-150~170-0.06(30).gcode` 的文件就匹配不上，历史卡片的 Filament/Layers 会全变 "--"。需要路径安全的是 `frames_dir`，它由 `make_session_name()` 生成，内部自带净化。

## 存储

环形预算（`ensure_video_storage_budget`，`statvfs(/userdata/media)`）：

- 录像目录占用 ≤ 分区容量的 `UPC_VIDEO_STORAGE_QUOTA_PCT`（默认 80%）
- 分区可用 ≥ `UPC_VIDEO_STORAGE_MIN_FREE_PCT`（默认 5%）
- 超出则删最旧**已提交**会话；保护 `tmp/`、在途 session、待 finalize 目录
- 先清孤儿/超时（30min）pending，再 eviction

写盘路径另挂 `storage_budget_guard`：预算不够则跳过该次 JPEG/H264 写。H264 耗尽时 `disable_h264_expectation()`，避免 `print_video_pending` 永久为 1。

**逐帧热路径必须走 `ensure_video_storage_budget_throttled()`，不能直连全量版。** 全量核算会
`recursive_directory_iterator` 遍历整个 `video/` 并 stat 每个文件，而层拍 JPEG 会累积到上千个：
按帧调用时开销随打印层数线性增长，实测能把 H264 编码 worker 压到只剩一半吞吐（30fps 送、
14fps 编，其余全进 `dropped`）。节流版只缓存「预算充足」的结论，窗口
`UPC_MEDIA_STORAGE_BUDGET_CACHE_MS`（默认 5s）；判定不足时**不缓存**，每次都重新核算，
免得一次瞬时不足误停整场录像。会话边界（Start / commit / finalize）仍用全量版求准。

`MEDIA_STORAGE_WARN_PCT` / `CRITICAL`（90/95）仍在头文件中，**当前未参与**开打水位；`apply_storage_watermark` 只尝试腾配额，层拍保持开启。

`UI_Storage_Usage_Req` → `APP_Storage_Usage_Rsp`（`statvfs` 总量/已用）。

## 拍照

`UI_Camera_Snapshot_Req` → 拷 latest NV12 → libturbojpeg（quality 80，`TJFLAG_FASTDCT`，4:2:0）  
`/userdata/media/photo/photo_{stamp}_{WxH}.jpg` + 同帧 RTHM（RGA 优先、CPU 兜底，见下节；失败不影响 JPEG）→ `APP_Camera_Snapshot_Rsp`。

不走 RK MPP：打印 H.264 与局域网 RTSP 已占 VPU。旧 `.png` 不再写入、也不再扫描播放。

依赖预览流已开；偶发空快照见日志。

## RTHM 缩略图双路径

`timelapse_util::create_raw_thumb_from_nv12` 是拍照、打印层拍、清粉录像、H264 录像
四条路径共用的唯一缩略图入口。它先算好几何（`dst_w` = `THUMB_MAX_W` 封顶、等比高、
`stride = dst_w*2`），再按顺序尝试两种实现，**两者产出的 `.rthm` 字节格式完全一致**：

| 路径 | 实现 | 何时使用 |
|---|---|---|
| RGA（优先） | `render_thumb_rga`，dma-heap + `importbuffer_fd` + `imcvtcolor_t`/`imresize` | `UPC_CAMERA_HAS_RGA` 且能拿到 RGA 可寻址内存 |
| CPU（兜底） | `render_thumb_cpu`，纯普通堆，Y 做 box 平均、UV 最近邻 | RGA 未编入或任一环节失败 |

缩略图与打印/直播编码都是先把 NV12 **拷出**相机 dma-buf，再进独立 dma-heap / `mpp_buffer`，不 `import` 正在给 ISP 用的 fd（与采集 IOMMU 的关系见 [camera/CAMERA.md](../camera/CAMERA.md) 「ISP IOMMU 与 dma-buf 所有权」）。

**为什么必须有 CPU 兜底**：RGA2 无 IOMMU，只能吃物理连续且 <4G 的内存（板端核对见 [common/COMMON.md](../../common/COMMON.md) `DmaHeapUsage`）。CMA 碎片化时
`/dev/dma_heap/reserved` 连几百 KB 连续内存都给不出（内核报 `cma_alloc: reserved:
alloc failed`），此时 RGA 路径整条失效。旧代码没有兜底，且 `dma_heap_alloc` 会静默
回退到页式 `system` 堆，结果是**缩略图静默不生成**——`snapshot thumb failed` 之外没有
任何线索。缩略图只有 240 px 宽，CPU 转换几毫秒，没有理由让它依赖 CMA 运气。

两条路径的色彩口径都是 **BT.601 limited range**，打包顺序对齐 LVGL `lv_color16_t`
（`blue:5/green:6/red:5`，小端下即 `(R<<11)|(G<<5)|B`），与 UI 侧
`raw_thumb_decoder` 直读一致，切换路径不会出现色差。

PHX 分块路径的 `kDstTileRows = 16`：1080×1280 源 + 240 宽缩略图下，原先的 64 行会让
中间 RGB565 块要到 154 页（616 KB）连续内存而稳定失败；16 行把峰值压到 41 页
（160 KB），代价是 RGA 调用从 5 次增到 18 次。

诊断：`importbuffer_fd failed` 日志会带每块 buffer 的**字节数与实际所在堆**。若堆是
`system` / `system-uncached`，说明是内存不在 RGA 可寻址范围，而非 RGA 本身故障。

## 打印层拍 / 延时

```
Glavo PrintVideoNotify → APP_MEDIA
  Start：配额腾挪 + recover/清 tmp → PrintVideoRecorder + PrintH264Recorder
  LayerSnap：latest NV12 → CPU JPEG（1.jpg,2.jpg,...）→ tmp/
  Finish/Abort：
    thumb.rthm（RGA 优先、CPU 兜底，见「RTHM 缩略图双路径」）
    upsert config.json（frames_dir=tmp，status=complete|terminated，video_pending）
    APP_Print_Video_Manifest_Ntf → UI（切结果页）
  结果页退出 UI_Print_Video_Finalize_Req：
    commit tmp → {job}/
    先 finalize 延时（清 video_pending）
    若开了连续 H264 且 encoder 未收尾，等 on_print_h264_finalize_done 再第二阶段
```

- `UPC_TIMELAPSE_PLAYBACK_PNG=1`（默认，宏名保留）：不编 `timelapse.mp4`，保留 JPEG；`=0` 则 JPEG→mp4，成功后可删清粉段 JPEG。
- 开了打印录像时用边录边封装的 `print_h264.mp4`（PTS 用**相机流 fps**，默认 30，勿用 layer_fps）。
- 0 帧：不写清单条目，但**仍发** `APP_Print_Video_Manifest_Ntf`（`has_entry=0` + `aborted`），否则 UI 停在打印中。仅删**空** `tmp/`，以免误删正在写的 `print_h264.mp4`。
- 结果页只等层间 Manifest，不依赖 H264 编完。
- **第二阶段（`finalize_print_video=true`）必须无条件清 `print_video_pending`。** 它只在 H264
  确定收尾后才执行（开门 finalize 等 `on_print_h264_finalize_done`，开机恢复时更是无会话在跑），
  没有「稍后再来一次」的机会。留着 1 会让该条目被 `reclaim_orphan_or_stale_pending_for_budget()`
  当成陈旧 pending，超 30min 连整个会话目录一起删掉，延时 JPEG 一并赔进去。mp4 不可用时
  让 `print_video` 保持为空即可，UI 自然不展示录像。

## 打印连续 H264

```
预览帧 → worker 拷贝 NV12 入队（满则丢帧，不阻塞取流）
  → RKMPP CBR 编 Annex-B → ffmpeg stdin 封装 tmp/print_h264.mp4
  → Finish/Abort：active=0 + 入队 Finalize（消息线程立刻返回）
```

与层拍 worker 并行。磁盘跟不上时日志 `print_h264 queue full drop=... enc=...`（5s 汇总一行，
首次丢帧立即打；逐帧打会反过来拖慢取流线程）。队列只有 3 帧（≈100ms），编码稳态跟不上就会
持续丢帧且不会补回，所以 `dropped` 持续上涨要当吞吐问题查，而不是偶发抖动。

收尾时 `stop_muxer_locked()` 关掉 stdin 后要等 ffmpeg 写完 mp4 尾部的 moov，上限
`UPC_PRINT_H264_MUX_FLUSH_TIMEOUT_MS`（默认 10s）。**等不够就强杀会留下只有 ftyp+mdat、
体积照样几百 MB 但播放器打不开的文件**；超时与「退出码非 0」在日志里分开打，便于判断
mp4 是否可信。finalize 侧不再只看「文件非空」，而是用 `timelapse_util::mp4_has_moov_box()`
走一遍顶层 box 链确认索引存在；解析不出结论（截断在 box 中间、读失败）时保守当可用。  
码率固定 `CAMERA_PRINT_H264_BITRATE`=2048kbps；不再读 `cal.csv`。会话里 `h264_bitrate=0` 时用该默认值。  
`h264_continuous_on=0` 时 recorder 直接 return。暂停：`APP_Print_H264_Record_Pause_Ntf`。

无 RKMPP 时该路径编译关闭。

## 清粉录像

`APP_Clean_Video_Session_*` 按 `record_kind` 分流：

- **`record_kind=0`（结果页）**：接续打印目录已有 `1..N.jpg`，按 timelapse fps 写 `(N+1).jpg…`。N=0 则清粉段不录。Stop 成功只更新末帧 thumb 与清粉范围；MP4 在 `UI_Print_Video_Finalize_Req` 与打印 JPEG 一起合成。用户关（`0xfe`）删清粉 JPEG。Finalize 会等清粉会话结束（`clean_video_active_` 栅栏）。
- **`record_kind=1`（空闲 Manual）**：复用 `PrintH264Recorder` 连续录 `print_h264.mp4`（强制开，不看调试页 Print H264 Rec）。Stop 后（MCU 完 / 失败 / 用户再点停）立刻 promote 进 Print History；0 帧不建可播条目。不走结果页开门 finalize。

JSON 字段名仍是 `clean_first_png` / `clean_last_png`（帧序号，不是格式）。

## 直播（云端 + 局域网）

两路共用 `H264StreamEncoder`，只编一次：

```
预览帧 → on_frame_nv12（单槽 latest-wins）
  → worker 按 1/fps 打 pts（同一帧共用）
  → worker：RKMPP CBR Annex-B（GOP=fps≈1s，bps_max=1.5×，每个 IDR 带 SPS/PPS）
  → sink 分发（同一 pts_us）：
      · LiveRtspPusher  → 入队 → writer write(ffmpeg stdin)
                        → -c copy -f rtsp -rtsp_transport tcp → push_url
      · LocalRtspServer → 有客户端才入队；积压/新 PLAY 丢到下一个 IDR
                        → live555：H264VideoStreamDiscreteFramer → RTP
```

- 码率**固定** `UPC_LIVE_H264_BITRATE`（默认 4Mbps，与打印录像的 2Mbps 分开），`rc:bps_max` = 1.5× 给 I 帧和大运动留突发；改码率需重开 MPP。云端 `max_bitrate_kbps` 不参与配置，仅与板端不同时打日志。
- **pts 按已编码帧 1/fps 递增**（墙钟起步，同一帧所有包共用）。latest-wins 丢帧后若用采帧墙钟，播放器会空等缺口，观感卡顿。MPP 拆包时各包必须同一 pts，否则 live555 误判 AU。落后墙钟超过 200ms 才跳一次，避免延迟无限堆。
- sink 互不连坐：一路失败只摘该路；MPP 失败才停整个编码会话。
- **sink 回调不得阻塞**：云端只入队，`write()` 在 writer 线程（管道 `O_NONBLOCK`，满则 `poll(POLLOUT)`）。队列超 `UPC_LIVE_PUSH_QUEUE_MAX_BYTES`（默认 1MB）时**整段丢到下一个 IDR**，不丢单包。
- 拆除顺序：**摘 sink → 停 writer → 关 ffmpeg fd**。先关 fd 会让 writer poll 到已回收描述符。
- live555 事件循环**常驻**；开关只挂/摘 `ServerMediaSession` 与 sink。除 `triggerEvent()` 外，live555 API 只能在事件循环线程；挂/摘经命令队列。
- 地址 `rtsp://<本机IP>:8554/live`（一期无鉴权，只打日志、不在 UI 展示）。
- **必须听 IPv4**（`0.0.0.0:8554`）。live555 默认 IPv4/IPv6 有一个成功就算起来；进程刚退出时 8554 会在 `TIME_WAIT`，IPv4 bind 失败仍可能只剩 `*:8554`，Snowpath/VLC 直连被拒。现在 IPv4 失败则 `init()` 失败并打 Error，IPv6 可有可无；`tick()` 每 2s 重试，不必重启整机。
- **强制 RTP-over-TCP**：`TcpOnlyRtspServer` 在 SETUP 判 Transport，非 `RTP/AVP/TCP` 回 **461**（`local_rtsp SETUP over UDP rejected`）。ffmpeg/VLC 会改 TCP 重试，客户端可不写 `-rtsp_transport tcp`。实现上用 `ConnAccessor` 取得 `handleCmd_unsupportedTransport()`（友元不随继承传递）。会话回收秒数 **0**（不靠 RTCP RR 探活）：交织 TCP 上 Snowpath 往往不发 RR，默认 65s 会被当成死连接拆掉。
- **PLAY 必须从 IDR 起画**：无人拉流不入队；首个客户端 SETUP 时清空队列并 `request_idr()`，积压满则整段丢到下一个 IDR（与云端 pusher 相同）。旧实现丢最旧单个 NAL，半个 GOP 灌进解码器会大面积灰屏，Snowpath 约 12s 超时再 `ended→started`。
- `lan_ipv4_list()` 跳过 loopback、DOWN、虚拟网卡（`dummy`/`docker`/`veth`/…）与 `169.254.0.0/16`，按 `UPC_WIFI_IFACE`（默认 `wlan0`）→ 其它无线 → 有线 → USB 排序。服务听 `0.0.0.0`；日志 `candidates=[...]`。
- `OutPacketBuffer::maxSize` 必须在任何 RTPSink 构造前设为 `UPC_LOCAL_RTSP_MAX_FRAME_BYTES`（默认 1MB）。live555 默认 60000，1080×1280 I 帧 150～200KB 会被静默截断。
- 上电默认开（`UPC_LOCAL_RTSP_AUTO_START=1`），实际拉起在流就绪后。`local_rtsp_desired_` 跟调试页开关；流未就绪时回 `on=0`，UI 开关弹回。
- 无 live555 时本地 Server 为空实现。

心跳每 5s（`h264_enc` 编码期间、`local_rtsp` 有客户端时始终打；`live_rtsp` 仍受 `UPC_MEDIA_LOG_STREAM_STAT` 控制）：

| 日志 | 关键字段 | 怎么读 |
|---|---|---|
| `h264_enc stat` | `sinks` / `in` / `enc` / `kbps` / `overwritten` | `in` 不涨=相机没送帧；`in` 涨而 `enc` 不涨=卡在 MPP 或某 sink 回调；`overwritten`=单槽被覆盖次数 |
| `live_rtsp stat` | `in` / `out` / `kbps` / `queue` / `dropped` / `drop_events` | `queue` 贴上限=云端吃不下；`in` 涨而 `out` 不涨=写端卡住 |
| `local_rtsp stat` | `in` / `out` / `queue` / `dropped` / `client` | `queue` 贴上限=live555 取不赢，随后丢到 IDR |

`live_rtsp` / `local_rtsp` / `h264_enc` → `topic_media_rtsp.log`；其余录制 → `topic_media_record.log`。live555 库诊断经 `LogUsageEnvironment` 成 `local_rtsp live555: …`（默认 stderr）。

## IPC

- **客户端**：`set_mode_ipc_client(APP_MEDIA)`，Broker 默认 `/tmp/upc_broker.sock`
- Glavo→Media 经 Hub 转发；`sender_id` 仍为 `APP_GLAVO`。`PrintVideoNotify` 目标 **`APP_MEDIA`**。
- 回复 UI：**必须** `publish_async()`，目标 `ModuleId::UI`

| MsgType | 动作 |
|---------|------|
| `MEDIA_Camera_Stream_Start_Req` / `Stop_Req` | →Camera 并行取流；Rsp 带 SHM 名 + dma fd |
| `UI_Camera_Snapshot_Req` | 存 JPEG + `.rthm` → `APP_Camera_Snapshot_Rsp` |
| `APP_Print_Video_Session_*` / `APP_Print_Layer_Snap_Ntf` | 打印录像会话 |
| `APP_Print_H264_Record_Pause_Ntf` | 暂停/恢复连续 H264 |
| `APP_Clean_Video_Session_*` | 清粉延时抽帧 |
| `APP_Print_Video_Manifest_Ntf` | Media→UI：manifest `rename()` 后发（0 帧也必发） |
| `UI_Print_Video_Finalize_Req` | 结果页退出：commit + 分阶段 finalize |
| `UI_Storage_Usage_Req` | → `APP_Storage_Usage_Rsp` |
| `MQTT_Media_Rtsp_Start/Stop_Req` | 云端 live；`APP_Mqtt_Rtsp_*_Rsp` 回 MQTT |
| `UI_Debug_Local_Rtsp_Set/Get_Ntf` | 局域网 RTSP（调试页，直发 `APP_MEDIA`） |
| `APP_Media_Local_Rtsp_State_Ntf` | Media→UI：实际开关（失败回 0） |

契约总表：[common/COMMON.md](../../common/COMMON.md)。

## 默认参数

| 项 | 默认 | 环境变量 / 宏 |
|----|------|---------------|
| 录像输出 | `/userdata/media/video/tmp/` → finalize `/userdata/media/video/{job}/` | `UPC_VIDEO_TMP` / `CAMERA_PRINT_VIDEO_OUT_DIR` |
| 缩略图 | `{dir}/thumb.rthm` | `CAMERA_PRINT_VIDEO_THUMB_EXT` |
| 缩略图最大宽 | 240 | `CAMERA_PRINT_VIDEO_THUMB_MAX_W` |
| 缩略图分块行数 | 16（PHX 路径） | `kDstTileRows`（`timelapse_util.cpp`） |
| 拍照 | `/userdata/media/photo/photo_{stamp}_{WxH}.jpg` + `.rthm` | `save_latest_jpeg_snapshot` |
| 历史清单 | `/userdata/media/video/config.json` | `UPC_VIDEO_CONFIG` |
| ffmpeg | `/usr/bin/ffmpeg` | `CAMERA_FFMPEG_BIN`（打印 H.264 封装 / 序列转 mp4；静图不走 ffmpeg） |
| 静图 JPEG 质量 | 80 | `CAMERA_STILL_JPEG_QUALITY` |
| 延时 fps | 5 | `CAMERA_PRINT_VIDEO_LAYER_FPS`（不读 cal.csv） |
| 延时播放 | JPEG 序列（`1.jpg`…） | `UPC_TIMELAPSE_PLAYBACK_PNG`（默认 1；宏名保留） |
| 录像配额 | 分区 80% / 余量 5% | `UPC_VIDEO_STORAGE_QUOTA_PCT` / `MIN_FREE_PCT` |
| 预算检查缓存窗口 | 5 s（仅缓存「充足」） | `UPC_MEDIA_STORAGE_BUDGET_CACHE_MS` |
| mp4 收尾等待上限 | 10 s | `UPC_PRINT_H264_MUX_FLUSH_TIMEOUT_MS` |
| 本地 RTSP 端口 | 8554 | `UPC_LOCAL_RTSP_PORT` |
| 本地 RTSP 流名 | `live` | `UPC_LOCAL_RTSP_STREAM_NAME` |
| 本地 RTSP 上电 | 开（流就绪后） | `UPC_LOCAL_RTSP_AUTO_START` |
| 本地 RTSP NAL 队列 | 48（满丢到下个 IDR；约 1s） | `UPC_LOCAL_RTSP_QUEUE_MAX` |
| 本地 RTSP 单帧上限 | 1 MB | `UPC_LOCAL_RTSP_MAX_FRAME_BYTES` |
| 云端发送队列 | 1 MB（满丢到下个 IDR） | `UPC_LIVE_PUSH_QUEUE_MAX_BYTES` |
| 打印录像码率 | 2048 kbps | `CAMERA_PRINT_H264_BITRATE`（不读 cal.csv） |
| 直播码率 | 4096 kbps | `UPC_LIVE_H264_BITRATE` |
| 直播 GOP | `fps`≈1s | `h264_stream_encoder.cpp` `rc:gop` |
| 直播码率峰值 | 1.5× 目标 | `rc:bps_max`（I 帧/大运动不掐死） |
| 心跳 | 5 s | `kStatsPeriodUs`；`UPC_MEDIA_LOG_STREAM_STAT`（默认 0） |

几何/帧率运行时跟 Camera 开流 IPC，不跟本表初值。

## 红线

- 禁止链接 `ui::port`、`upc::glavo`、lvgl
- 回复 UI 必须 `publish_async()`，目标 `ModuleId::UI`
- 改消息契约：[common/COMMON.md](../../common/COMMON.md) 四步

## 相关文档

- [camera/CAMERA.md](../camera/CAMERA.md) · [common/COMMON.md](../../common/COMMON.md) · [ui/UI.md](../ui/UI.md) · [glavo/GLAVO.md](../glavo/GLAVO.md)

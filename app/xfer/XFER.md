# xfer/ — WiFi 传输进程（rongsu_xfr）

> 可执行文件：`rongsu_xfr`  
> 命名空间：`upc::xfr`

## 定位

- 提供局域网 mDNS 发现（`_snowpod._tcp`）和 HTTP 文件上传服务。
- 负责配对鉴权、`*.ovf` / 传输 `*.3mf` 上传、后处理与本地打印清单更新。
- OVF：上传到 `UPC_XFR_STORAGE_DIR` 后运行 `snowslice`，输出 `*_out.ovf` 到
  `UPC_LOCAL_GCODE_DIR`（默认 `/userdata/Gcode`），原有客户端流程保持不变。
- 3MF：整包 SHA-256 通过后，从 `3D/*.gcode`（兼容
  `Metadata/*.gcode`）提取**唯一一份**已切片 G-code；不运行 `snowslice`。
  ZIP 路径、条目数、压缩方法、CRC、大小与压缩比均需通过校验。
  必须读取 `Metadata/*.json`（`snowpath_plate_meta_v1`）的 `layers` /
  `print_time_s` 写入清单；`line_count` 固定为 0（打印走纯层进度）。
- 两种格式完成后都更新 `UPC_LOCAL_GCODE_CONFIG`，供 UI Local 列表与打印流程使用。

## API（`/api/v1`）

- HTTP：支持 HTTP/1.1 keep-alive（Snowpath 在 `complete` 后复用同一条连接发
  `print/start`）。每个连接线程 `detach`，避免状态轮询堆积未 join 的线程把板打挂。
- `GET /info`：返回 `sn/model/api_version/need_pair/state`
- `POST /pair/start`：生成 PIN 与配对会话
- `POST /pair/verify`：校验 PIN 并签发 token
- `POST /pair/revoke`：作废当前 token
- `POST /upload/init`：创建上传会话
- `PUT /upload/{id}`：按 `Content-Range` 分块上传
- `POST /upload/{id}/complete`：整文件 SHA-256 校验后按扩展名分流：
  `.ovf` 走 snowslice/OVF 预处理；`.3mf` 解出单个 `.gcode`；最后写入 `config.json`
- `GET /upload/{id}/status`：查询断点续传进度
- `GET /print/status`：桌面端轮询打印机状态/进度（传输态本地维护；打印态订阅
  Glavo/Media IPC 缓存）。传输完成对外为 `idle`；`finished` 仅表示打印完成。
  打印中 `job_id` 优先为延时摄影 `frames_dir`（打印中多为 `tmp`）。
- `POST /print/start`：对刚导入的 Local 文件发与 UI 相同的
  `UI_Print_Prepare_Req` → `UI_Print_Start_Req`（等 Prepare 回包，最多 5s）。
  成功回 `200 {"state":"printing"}`。已在打印中 `409 PRINTER_BUSY`；
  清单对不上 `404 FILE_NOT_FOUND`；Prepare 失败 `500` / 超时 `504`。
  板端 Skip / Keep Z / 外门确认仍走 Glavo 原门控。`GET /print/status` 仍以
  Glavo 打印态为准。
- `POST /print/cancel` / `/print/stop` / `/print/pause` / `/print/resume`：body 仅 `job_id`
  （与 6.3 相同：优先 `frames_dir`，打印中多为 `tmp`，也认 6.2 的 `j_*`；
  延时摄影清单到达后仍保留原来的 `j_*`，避免切片软件 cancel 404）。
  校验后向 Glavo 发与 UI 相同的 Terminate/Pause/Resume，等回包再 `200`。
  `job_id` 对不上 `404 JOB_NOT_FOUND`；态不对 `409 JOB_NOT_PRINTING`；
  取消等回包最多 15s，超时仍 `200 canceled`（Terminate 已发出）。
  取消允许会话已起或 `printing`/`paused`；暂停仅 `printing`；恢复仅 `paused`
  （氧未落下仍暂停则 `409`）。
- `GET /print/history?year=&month=&day=`：按打印机本地日期返回当天 `job_ids`
  （= `/userdata/media/video/config.json` 的 `frames_dir`，升序）
- `GET /print/{job_id}/timelapse/images`：分页返回该任务延时摄影 JPEG URL 列表
- `GET /timelapse/{job_id}/{n}.jpg`：匿名下载已落盘帧（列表接口已鉴权后的产物）

### 打印状态 / 延时摄影（板端约定）

- 历史与延时摄影的 `job_id` 使用清单主键 `frames_dir`，不是协议示例里的 `j_9001`。
- 帧文件在 `/userdata/media/video/{frames_dir}/N.jpg`；URL 扩展名为 `.jpg`（与 new-ui 静图一致）。
- `/print/status` 中 `chamber_temp` 来自 Glavo 进度载荷的舱温（0.1℃）；
  `layer_height` 来自进度载荷 `layer_height_um`（毫米，打印中有值，空闲为 `0`）；
  `nozzle_temp` / `bed_temp` / `laser_power` / `blower_*` / `filament_*`
  本期固定为 `0`。
- 数据源：`APP_Print_Progress_Ntf` / `APP_Print_Pause_Rsp` /
  `GLAVO_Print_State_Ntf` / `APP_Print_Terminate_Rsp` /
  `APP_Print_Start_Rejected_Ntf` / `APP_Print_Video_Manifest_Ntf`（Glavo/Media
  双发 UI 与 `APP_XFR`）。新任务已在 `printing`/`paused` 时，上一趟取消的
  Manifest `aborted` 与迟到 `Terminate_Rsp` 不得把对外 state 写回 `canceled`。

## 运行参数（环境变量）

- `UPC_XFR_PORT`（默认 `8080`）
- `UPC_XFR_STORAGE_DIR`（默认 `/userdata/transport`）
- `UPC_XFR_GCODE_DIR`（默认 `UPC_LOCAL_GCODE_DIR`，即 `/userdata/Gcode`）
- `UPC_XFR_TOKEN_FILE`（默认 `/userdata/transport/.xfr_tokens`）
- `UPC_XFR_REQUIRE_AUTH`（默认 `1`）
- `UPC_XFR_CHUNK_MAX`（默认 `4194304`）
- `UPC_DEVICE_SN` / `UPC_DEVICE_MODEL`

## 传输 3MF

已联调样例：

```text
竖放_0_transfer.3mf
├── 3D/竖放_0.gcode
├── Metadata/竖放_0.json
└── Thumbnails/竖放_0.png
```

当前板端约定：

- 包内必须且只能有一份 `.gcode`。
- 3MF 压缩包最大 512 MiB，解压后的 G-code 最大 1 GiB，压缩比最大 200。
- G-code 路径为 `3D/*.gcode`；兼容早期草案的 `Metadata/*.gcode`。
- 输出名取上传包名去掉 `.3mf` 后的 ASCII stem 再加 `.gcode`。只保留
  `[A-Za-z0-9_-]`；含中文/日文等非 ASCII 时变成
  `model_<8hex>_<残留ASCII>.gcode`，避免 UI 缺字乱码。
- 上传暂存目录为 `/userdata/transport`（分块 `.part.*` 与完整 `.3mf`）；
  导入成功后**删除**原 3MF。可打印文件落到 `/userdata/Gcode`，并写入
  `/userdata/Gcode/config.json`。
- 必须包含 `Metadata/*.json`（优先与 G-code 同 stem；仅一份时直接使用）。
  读取 `layers` → `layer_count`（必须 >0，否则拒绝导入）、
  `print_time_s` → `duration_sec`；`line_count` 写 0。
- `Thumbnails/*.png` 优先取与 G-code 同 stem 的图片；包内只有一张 PNG
  时直接使用。PNG 仅作解码缓存：生成三份 UI 可直接读取的 RGB565 RTHM 后删除：
  `/userdata/Gcode/thumb/*_small.rthm`（72×72）、
  `*_large.rthm`（320×180）、`*_print.rthm`（100×100），绝对路径分别写入
  manifest 的 `thumb_small` / `thumb_large` / `thumb_print`。
- PNG 解码使用 libpng，长边抽到 1280（大于目标 320，给留白裁切留像素）。
  Snowpath 预览四周常是大片底色，模型只占画布中心一小块：先抹掉青绿支撑色
  （旧灰底预览；蓝底同色支撑与模型分不开，不在板端滤），再按四角底色
  抠内容框（约 10% 边），再按目标比例 CONTAIN 放进 72 / 320×180 / 100
  （整模可见；比例不同时用原底色补边，避免竖模被 16:9 COVER 切掉头脚）。
  四角颜色不一致或内容过小则退回整图。不保留源 PNG。
  缩放优先走 RGA（dma32 + `importbuffer_fd`，禁止
  `wrapbuffer_virtualaddr`：RK3568 RGA2 无 IOMMU，普通堆虚地址会整进程
  退出）。超过 RGA2 的 8 倍缩放时先落到中间尺寸再出目标图；dma-heap /
  导入 / 硬件失败回退 CPU。缺图、图片损坏或转换失败只记录告警，G-code
  仍正常导入，三个 manifest 字段留空。
- G-code、三份 RTHM 与 manifest 通过临时文件 + rename 提交；
  同名重传会替换旧资产。删除 Local 文件时同步删除对应 RTHM，
  删除范围仅开放 `/userdata/Gcode/thumb`。
- 样例没有 `.gcode.md5`，因此完整性由上传整包 SHA-256、ZIP CRC 和解压后大小共同保证。

本机冒烟测试是独立 CMake 工程（不要使用交叉工具链）：

```bash
sudo apt install zlib1g-dev libpng-dev
cmake -S app/xfer/tests -B /tmp/upc-xfr-tests
cmake --build /tmp/upc-xfr-tests
/tmp/upc-xfr-tests/transfer_3mf_smoke doc/竖放_0_transfer.3mf 796
```

该 smoke 同时检查正常 triplet（无残留 PNG）、损坏 PNG 降级并清理旧图、
缺图仍导入、缺 Metadata 拒绝四种情况。

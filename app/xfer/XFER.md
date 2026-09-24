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
- `POST /print/start`：本期不代发起印（仍由板端 UI 点 Confirm）。回包 `state`
  只看 Glavo 打印态，**不**回传上传线内部的 `finished`（否则 Snowpath 会当成
  打印已结束，6.3 不再刷层高/进度/舱温）。已在打印中回 `409 PRINTER_BUSY`。
- `GET /print/history?year=&month=&day=`：按打印机本地日期返回当天 `job_ids`
  （= `/userdata/media/video/config.json` 的 `frames_dir`，升序）
- `GET /print/{job_id}/timelapse/images`：分页返回该任务延时摄影 PNG URL 列表
- `GET /timelapse/{job_id}/{n}.png`：匿名下载已落盘帧（列表接口已鉴权后的产物）

### 打印状态 / 延时摄影（板端约定）

- 历史与延时摄影的 `job_id` 使用清单主键 `frames_dir`，不是协议示例里的 `j_9001`。
- 帧文件在 `/userdata/media/video/{frames_dir}/N.png`；URL 扩展名为 `.png`。
- `/print/status` 中 `chamber_temp` 来自 Glavo 进度载荷的舱温（0.1℃）；
  `layer_height` 来自进度载荷 `layer_height_um`（毫米，打印中有值，空闲为 `0`）；
  `nozzle_temp` / `bed_temp` / `laser_power` / `blower_*` / `filament_*`
  本期固定为 `0`。
- 数据源：`APP_Print_Progress_Ntf` / `APP_Print_Pause_Rsp` /
  `GLAVO_Print_State_Ntf` / `APP_Print_Terminate_Rsp` /
  `APP_Print_Start_Rejected_Ntf` / `APP_Print_Video_Manifest_Ntf`（Glavo/Media
  双发 UI 与 `APP_XFR`）。

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
- 输出名取上传包名去掉 `.3mf` 后加 `.gcode`，例如
  `竖放_0_transfer.gcode`。
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
- PNG 解码使用 libpng，按 COVER 居中裁剪；有 librga 时优先由 RGA
  缩放并转 RGB565，失败自动走 CPU。缺图、图片损坏或转换失败只记录告警，
  G-code 仍正常导入，三个 manifest 字段留空。
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

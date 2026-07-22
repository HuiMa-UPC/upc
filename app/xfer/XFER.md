# xfer/ — WiFi 传输进程（rongsu_xfr）

> 可执行文件：`rongsu_xfr`  
> 命名空间：`upc::xfr`

## 定位

- 提供局域网 mDNS 发现（`_snowpod._tcp`）和 HTTP 文件上传服务。
- 仅负责配对鉴权与文件落盘，不做文件解析和打印控制。
- 当前仅接收 `*.json`、`*.svg`，传输完成后落盘到 `/home/transport`。
- 由 `POST /build/start` 触发 `snowslice_demo` 生成 gcode，并更新 `/home/Gcode/config.json`。
- 构建成功后删除本次输入的 `*.json`、`*.svg`（构建失败保留原文件）。

## API（`/api/v1`）

- `GET /info`：返回 `sn/model/api_version/need_pair/state`
- `POST /pair/start`：生成 PIN 与配对会话
- `POST /pair/verify`：校验 PIN 并签发 token
- `POST /pair/revoke`：作废当前 token
- `POST /upload/init`：创建上传会话
- `PUT /upload/{id}`：按 `Content-Range` 分块上传
- `POST /upload/{id}/complete`：整文件 SHA-256 校验与落盘；成功后自动触发构建
- `GET /upload/{id}/status`：查询断点续传进度
- `POST /build/start`：手动补触发（可选）；默认由 upload complete 自动触发构建
- `GET /build/status`：查询构建状态（`idle/running/success/failed`）

## 运行参数（环境变量）

- `UPC_XFR_PORT`（默认 `8080`）
- `UPC_XFR_STORAGE_DIR`（默认 `/home/transport`）
- `UPC_XFR_GCODE_DIR`（默认 `/home/Gcode`）
- `UPC_XFR_TOKEN_FILE`（默认 `/home/transport/.xfr_tokens`）
- `UPC_XFR_REQUIRE_AUTH`（默认 `1`）
- `UPC_XFR_CHUNK_MAX`（默认 `4194304`）
- `UPC_XFR_SNOWSLICE_BIN`（默认 `/home/snowslice_demo`）
- `UPC_XFR_BUILD_TIMEOUT_S`（默认 `120`）
- `UPC_DEVICE_SN` / `UPC_DEVICE_MODEL`

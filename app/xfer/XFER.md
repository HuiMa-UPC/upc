# xfer/ — WiFi 传输进程（rongsu_xfr）

> 可执行文件：`rongsu_xfr`  
> 命名空间：`upc::xfr`

## 定位

- 提供局域网 mDNS 发现（`_snowpod._tcp`）和 HTTP 文件上传服务。
- 负责配对鉴权、`*.ovf` 上传、OVF 预处理与本地打印清单更新。
- 当前仅接收 `*.ovf`，上传完成后文件落盘到 `UPC_LOCAL_GCODE_DIR`（默认 `/userdata/Gcode`）。
- 接收完成后立即执行 OVF 预处理并更新 `UPC_LOCAL_GCODE_CONFIG`（供 UI Local 列表与打印流程使用）。

## API（`/api/v1`）

- `GET /info`：返回 `sn/model/api_version/need_pair/state`
- `POST /pair/start`：生成 PIN 与配对会话
- `POST /pair/verify`：校验 PIN 并签发 token
- `POST /pair/revoke`：作废当前 token
- `POST /upload/init`：创建上传会话
- `PUT /upload/{id}`：按 `Content-Range` 分块上传
- `POST /upload/{id}/complete`：整文件 SHA-256 校验、移动到 `UPC_LOCAL_GCODE_DIR`、预处理并写入 `config.json`
- `GET /upload/{id}/status`：查询断点续传进度

## 运行参数（环境变量）

- `UPC_XFR_PORT`（默认 `8080`）
- `UPC_XFR_STORAGE_DIR`（默认 `/userdata/transport`）
- `UPC_XFR_GCODE_DIR`（默认 `UPC_LOCAL_GCODE_DIR`，即 `/userdata/Gcode`）
- `UPC_XFR_TOKEN_FILE`（默认 `/userdata/transport/.xfr_tokens`）
- `UPC_XFR_REQUIRE_AUTH`（默认 `1`）
- `UPC_XFR_CHUNK_MAX`（默认 `4194304`）
- `UPC_DEVICE_SN` / `UPC_DEVICE_MODEL`

# xfer/ — WiFi 文件传输进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)
> 可执行文件：**`rongsu_xfer`** · 库 **`upc_xfer`** · 命名空间 `upc::xfer`
> 协议来源：[doc/WIFI-TRANSFER.md](../../doc/WIFI-TRANSFER.md)（本进程实现其中「固件服务端」的传输子集）

## 定位

PC 端（切片软件）通过局域网 WiFi 把「传输 3mf」用 HTTP **分块上传**到设备；本进程负责
**接收 + 鉴权 + 校验 + 解包**，把每盘 gcode 落到 `/home/Gcode`。**只管收文件和解析，不含打印逻辑**：
落盘后由 Glavo 既有的「列目录 → 选文件 → 打印」链路接手，两者经**文件系统解耦**。
本进程作为**可选 IPC 后端**（`ModuleId::APP_XFER`），仅**单向**通知 UI（传输完成刷新列表 /
屏幕显示配对 PIN），不接收任何 IPC 指令。

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 进程守护 `{start\|stop\|status\|run}`，起 `XferService`，主线程等退出信号 |
| `lib/xfer_service.*` | 编排：建目录 → 起 HTTP → 发布 mDNS |
| `lib/http_server.*` | CivetWeb 封装 + 路由：`/info`、`/upload/{init,PUT,complete,status}`；complete 后落地 gcode |
| `lib/upload_manager.*` | 上传会话：分块写 `.part`、流式 SHA-256、整体校验、原子 rename（线程安全） |
| `lib/transfer_3mf.*` | miniz 解 3mf，按 `.gcode.md5` 校验，抽 `Metadata/plate_<id>.gcode` → `/home/Gcode` |
| `lib/auth.*` | PIN→Token 配对（`PairManager`）+ Token 持久化（`TokenStore`） |
| `lib/ipc_notifier.*` | 单向通知 UI：传输完成 / 屏幕 PIN（经 `publish_async`） |
| `lib/hash_util.*` | OpenSSL 封装：流式 SHA-256 + 单盘 MD5 |
| `lib/mdns_publisher.*` | 写 avahi 静态服务文件发布 `_snowpod._tcp`（不链 libavahi） |
| `lib/xfer_config.*` | 端口 / 目录 / SN / 鉴权开关 / 分块上限，支持环境变量覆盖 |

## 数据流

```
PC ──①mDNS 发现 _snowpod._tcp──► avahi-daemon（读 /etc/avahi/services/snowpod.service）
PC ──②HTTP 分块上传──► rongsu_xfer
        upload/init → PUT(Content-Range) → complete(整文件 SHA-256)
        └─ .part 原子 rename 为 <staging>/<name>.3mf
        └─ miniz 解包 + 单盘 MD5 校验 → /home/Gcode/<stem>_plate_<id>.gcode
/home/Gcode ──③Glavo 既有 Fs_DirOpen 扫描──► UI 文件列表 ──► 打印
```

## HTTP 端点（基路径 `/api/v1`，v1 局域网明文）

| 方法 | 路径 | 鉴权 | 说明 |
|------|------|------|------|
| GET | `/info` | 否 | `sn/model/api_version/need_pair/state`（`need_pair`=`require_auth`） |
| POST | `/pair/start` | 否 | body `{client_name}` → `{pair_session,expires_in}`；并 IPC 通知 UI 显示 PIN |
| POST | `/pair/verify` | 否 | body `{pair_session,pin,device_name}` → `{token,sn,name}` |
| POST | `/pair/revoke` | Bearer | 作废当前 Token |
| POST | `/upload/init` | Bearer | body `{filename,size,sha256}` → `{upload_id,offset,chunk_max}` |
| PUT | `/upload/{id}` | Bearer | header `Content-Range: bytes s-e/total`，body 原始字节 → `{offset}` |
| POST | `/upload/{id}/complete` | Bearer | body `{sha256}` → `{file_id,size,verified,plates}`；触发解包 + 通知 UI |
| GET | `/upload/{id}/status` | Bearer | `{offset,size,state}` 断点续传查询 |

鉴权（[WIFI-TRANSFER.md §4](../../doc/WIFI-TRANSFER.md)）：`require_auth=true` 时上传端点需
`Authorization: Bearer <token>`。配对屏幕 PIN 为 4 位、120s 时效、错错 5 次锁定会话；
Token 持久化于 `token_file`，重启后免再配对。

错误码对接 [WIFI-TRANSFER.md §8](../../doc/WIFI-TRANSFER.md)：`AUTH_INVALID`(401)、
`PAIR_BAD_PIN`(401)、`PAIR_EXPIRED`(410)、`CHECKSUM_MISMATCH`(422)、`UPLOAD_NOT_FOUND`(404)、
`OFFSET_MISMATCH`(422)、`UNPACK_FAILED`(422)、`IO_ERROR`(500)。

## IPC 通知（XFER → UI，单向，可选）

作为可选后端连接 UI hub（socket `/tmp/upc_xfer.sock`，UI 侧 `add_ipc_peer(APP_XFER, required=false)`）：

| MsgType | Payload | UI 动作 |
|---------|---------|---------|
| `XFER_File_Received_Ntf` | `XferFileReceivedPayload{file_id, plates}` | toast「File received…」+ `send_fs_dir_open()` 刷新列表 |
| `XFER_Pair_Pin_Ntf` | `XferPairPinPayload{pin, expires_s}` | toast 显示配对 PIN |

`publish_async` 从 CivetWeb 工作线程入队，main 循环 `MsgPort::process()` 投递；UI 未连接时丢弃，不影响传输。
UI 回调在 `control.cpp` 绑定（`register_xfer_file_received` / `register_xfer_pair_pin`）。
> 注：当前 UI 字体为 ASCII，toast 文案用英文；若启用 CJK 字体可改中文。

## 落地规则（`complete` 之后）

- 文件名 `*.3mf`：当作传输 3mf 解包，输出 `<stem>_plate_<id>.gcode`（`stem` 去掉 `_transfer`）。
- 文件名 `*.gcode` / `*.nc`：视为裸 gcode，直接搬运到 `/home/Gcode`。
- 其它：保留在 staging，不视为失败。

## 配置（环境变量，缺省见 `xfer_config.hpp`）

| 变量 | 缺省 | 说明 |
|------|------|------|
| `UPC_XFER_PORT` | `8080` | HTTP 监听端口 |
| `UPC_XFER_STAGING_DIR` | `/home/transfer` | `.part`、上传完的 3mf、`.xfer_tokens` 落点 |
| `UPC_XFER_GCODE_DIR` | `/home/Gcode` | 解包后单盘 gcode 落点（须与 Glavo 列目录一致） |
| `UPC_XFER_REQUIRE_AUTH` | `1` | `0` 关闭 PIN/Token 鉴权（裸上传） |
| `UPC_XFER_IPC_SOCKET` | `/tmp/upc_xfer.sock` | IPC listen socket（UI hub 连接） |
| `UPC_DEVICE_SN` / `UPC_DEVICE_MODEL` | 占位 | mDNS / `GET /info` 暴露 |

## 设备发现（mDNS）

启动时把 `_snowpod._tcp` 服务写入 `/etc/avahi/services/snowpod.service`，由设备上的
**avahi-daemon** 自动广播。**前提：设备 rootfs 安装并运行 avahi-daemon**；若没有或无写权限，
仅告警、不致命——客户端可用「手动输 IP」兜底（[WIFI-TRANSFER.md §3.3](../../doc/WIFI-TRANSFER.md)）。

## 红线

- **与 Glavo 经 `/home/Gcode` 文件系统解耦**：不向 Glavo 发任何 IPC。
- **IPC 仅单向通知 UI**（`XFER_*_Ntf`），不接收任何 IPC 指令、不参与打印控制。
- **不含打印逻辑**：不下发 start/cancel，不查打印状态。
- 不链接 lvgl、不链接 `app/glavo` 业务库。

## 依赖

| 库 | 版本 | 许可证 | 用途 |
|----|------|--------|------|
| [CivetWeb](https://github.com/civetweb/civetweb) | v1.16 | MIT | HTTP 服务端（`3rdparty/civetweb/`，编译期 `NO_SSL`/`NO_CGI`） |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | MIT | 解包 3mf（ZIP）（`3rdparty/miniz/`） |
| OpenSSL libcrypto | 系统 sysroot | Apache-2.0/OpenSSL | SHA-256 / MD5（链 `OpenSSL::Crypto`） |

> v1 范围：明文 HTTP + PIN/Token 鉴权 + 传输/解包 + 单向 UI 通知。TLS / 打印端点为后续增强。

## 编译说明

- 默认工具链 `k1mini-buildroot-toolchain`（sysroot 含 protobuf/openssl）；`./build.sh` 即可，产物含 `rongsu_xfer`。
- 备用工具链 `atk-dlrk356x-toolchain` 的 sysroot **缺 protobuf 头**，会导致预存的 OVF 解析器编译失败（与本进程无关）。

## 相关文档

- [glavo/GLAVO.md](../glavo/GLAVO.md)（下游打印）· [parser/PARSER.md](../parser/PARSER.md)（gcode 后缀注册）· [doc/WIFI-TRANSFER.md](../../doc/WIFI-TRANSFER.md)（协议）

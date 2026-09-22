# 存储管理 — 后端接口交接

设置页「存储管理」（稿 `6251:94362`）**前端已经做完**，后端还没开始。这份文档给要
接手的同事：哪几条消息、字段什么意思、UI 已经做到哪一步、你需要补什么。

契约定义在 [common/upc_contract.hpp](../common/upc_contract.hpp)，总表在
[common/COMMON.md](../common/COMMON.md)，UI 侧接线在
[ui_msg_proxy.cpp](../app/ui/lib/core/src/ui_msg_proxy.cpp) 与
[page_user_settings.cpp](../app/ui/lib/pages/src/page_user_settings.cpp)。

## 卷枚举

```c
enum class StorageVolume : uint8_t { Internal = 0, SdCard = 1, UsbDisk = 2 };
```

| 卷 | 挂载点 | 现状 |
|----|--------|------|
| `Internal` | `/userdata`（`UPC_MEDIA_ROOT` 所在分区） | 容量查询已实现 |
| `SdCard` | **待定** | 未实现。设备上 SD 卡怎么枚举、挂到哪，需要先定 |
| `UsbDisk` | 由 `GLAVO_USB_Hotplug_Ntf` 的 `mount_path` 给出 | 挂载检测已实现，格式化未实现 |

UI 上这三个卷出现在不同位置：**存储概览是 Internal + SdCard 两张卡，格式化是
Internal + UsbDisk 两行**，不要按「同一批卷」处理。

## 一、容量查询（部分实现）

```
UI  → Media   UI_Storage_Usage_Req    StorageUsageReqPayload { volume }
Media → UI    APP_Storage_Usage_Rsp   StorageUsagePayload { used_bytes, total_bytes, volume, present }
```

`present` 不能省：没有它，UI 分不清「卷在、确实是空的」和「卷压根没插」，两种情况
`used_bytes` 都是 0。UI 只在 `present=1` 时画容量条，否则显示 `--`。

**Media 侧现状**（[media_service.cpp](../app/media/lib/media_service.cpp) 的
`handle_storage_usage_req`）：`Internal` 走 `statvfs(UPC_MEDIA_ROOT)` 回真实值并置
`present=1`；`SdCard` / `UsbDisk` 直接回 `present=0`。

**要补的**：`SdCard` 那一支——确定挂载点后照 `Internal` 那段加个分支即可。

> 请求方不带载荷时按 `Internal` 处理，这是为了不动历史页那个老调用方。

## 二、格式化（完全未实现）

```
UI  → Media   UI_Storage_Format_Req            StorageFormatReqPayload   { volume }
Media → UI    APP_Storage_Format_Progress_Ntf  StorageFormatProgressPayload { volume, percent }   ← 反复发
Media → UI    APP_Storage_Format_Rsp           StorageFormatRspPayload   { volume, result }       ← 终态一次
```

`result`：`0`=成功 `1`=失败 `2`=忙（另一次格式化在跑） `3`=卷不存在。

拆成 Progress + Rsp 两条，是因为**格式化可能要几十秒**，只回一个终态的话 UI 在这段
时间里没法给任何反馈。Progress 的发送频率由后端定，UI 每收一条刷一次 toast，不做
节流也不做超时——**目前没有超时兜底，后端不回 Rsp 的话 UI 会一直停在最后那条进度上**，
这是已知的缺口，等后端行为定了再补。

**UI 侧已做**：二次确认弹窗（稿 `6283:26740`，复用历史页那个浅色档 496×231 弹窗）→
确认后发 `UI_Storage_Format_Req` → 进度 toast → 终态 toast，成功后自动重查一次容量。
后端不在的当下，确认后会出一条 `Format requested (backend pending)` 的 toast。

**要补的**：整条链路。注意几点：

- 格式化 `Internal` 等于抹掉 `/userdata`，**包括正在运行的进程的数据目录**，实现前
  先想清楚是真格式化分区还是只清 `/userdata/media` 下的用户数据。UI 文案写的是
  "Erases all data on the device"。
- U 盘的挂载点从 `GLAVO_USB_Hotplug_Ntf` 拿，格式化前要先 umount。
- `result=2`（忙）要真的挡住并发请求，UI 不做本地互斥。

## 三、SD 卡热插拔（完全未实现）

```
Glavo → UI    GLAVO_SdCard_Hotplug_Ntf    UsbHotplugPayload { present, mounted, mount_path }
```

载荷形状与 `GLAVO_USB_Hotplug_Ntf` **完全一致**，故复用同一个 POD；但这是**另一个卷**，
UI 侧的回调也是分开注册的（`register_sdcard_hotplug`）。

**UI 侧已做**：订阅已建好，回调注册接口已开放。目前设置页还没用它——存储概览那张
SD 卡的 `present` 是靠容量查询回来的，等这条通了可以改成热插拔驱动、免得靠轮询。

**要补的**：Glavo 侧的 SD 卡挂载监听 + 这条通知。前提是先定挂载点。

## UI 侧没做的（不在本次范围）

- **文件管理两行**（Album / Models）只画了卡片，点了出 "Not implemented"。去向待定。
- **熄屏时间**是纯 UI 占位，选项只存在内存里，不落盘、也不真的熄屏。要真做得先定
  熄屏策略（空闲计时 + 背光置 0 或 sysfs blank + 触摸唤醒）。

# 存储管理 — 后端接口交接

设置页「Storage」（新稿 `6444:53065` / `6443:52816`，格式化确认 `6444:53244`）**前端已经
做完**，后端还没开始。这份文档给要接手的同事：哪几条消息、字段什么意思、UI 已经做到哪一步、
你需要补什么。

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

UI 上的用法：存储页顶部是 **Internal** 一张容量卡；下面「Connected External Storage」
列出**在位**的外部卷（U 盘按热插拔、SD 卡按热插拔或容量回执 `present=1`），没有外部卷就
整段不画；「Format」是 Internal 一行 + 每个在位的外部卷一行。外部卷的在位还被 Preferences
的「Recording Storage」与 About 的「Export Logs」两个选择弹窗用到（两者目前都只做界面）。

## 一、容量查询（部分实现）

```
UI  → Media   UI_Storage_Usage_Req    StorageUsageReqPayload { volume }
Media → UI    APP_Storage_Usage_Rsp   StorageUsagePayload { used_bytes, total_bytes, volume, present }
```

`present` 不能省：没有它，UI 分不清「卷在、确实是空的」和「卷压根没插」，两种情况
`used_bytes` 都是 0。UI 只在 `present=1` 时画容量条 / 可用容量，否则显示 `--`；SD 卡的
`present=1` 还会让它出现在外部存储列表里。

**Media 侧现状**（[media_service.cpp](../app/media/lib/media_service.cpp) 的
`handle_storage_usage_req`）：`Internal` 走 `statvfs(UPC_MEDIA_ROOT)` 回真实值并置
`present=1`；`SdCard` / `UsbDisk` 直接回 `present=0`。

**要补的**：`SdCard` 那一支——确定挂载点后照 `Internal` 那段加个分支即可；`UsbDisk`
同理，挂载点用 `GLAVO_USB_Hotplug_Ntf` 的 `mount_path`（UI 在 U 盘插入时会主动问一次）。

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

**UI 侧已做**：二次确认弹窗（稿 `6444:53244`，496 宽，Cancel + 红色 Format）→
确认后发 `UI_Storage_Format_Req` → 进度 toast → 终态 toast，成功后自动重查一次容量。
后端不在的当下，确认后会出一条 `Format requested (backend pending)` 的 toast。

**要补的**：整条链路。注意几点：

- 格式化 `Internal` 等于抹掉 `/userdata`，**包括正在运行的进程的数据目录**，实现前
  先想清楚是真格式化分区还是只清 `/userdata/media` 下的用户数据。UI 文案写的是
  "Erase all data on the device"。
- U 盘的挂载点从 `GLAVO_USB_Hotplug_Ntf` 拿，格式化前要先 umount。
- `result=2`（忙）要真的挡住并发请求，UI 不做本地互斥。

## 三、SD 卡热插拔（完全未实现）

```
Glavo → UI    GLAVO_SdCard_Hotplug_Ntf    UsbHotplugPayload { present, mounted, mount_path }
```

载荷形状与 `GLAVO_USB_Hotplug_Ntf` **完全一致**，故复用同一个 POD；但这是**另一个卷**，
UI 侧的回调也是分开注册的（`register_sdcard_hotplug`）。

**UI 侧已做**：订阅已建好，设置页已注册（单槽，只有它挂），`UiMsgProxy::sdcard_mounted()`
缓存最近一次。SD 卡在位取「这条报已挂载」或「容量回执 `present=1`」之一，这条通了就不必
靠进页时问容量。

**要补的**：Glavo 侧的 SD 卡挂载监听 + 这条通知。前提是先定挂载点。

## 四、U 盘当前态（没有查询消息）

Glavo 只在自己启动时和挂载变化时推一次 `GLAVO_USB_Hotplug_Ntf`。UI 的
`UiMsgProxy::usb_mounted()` 缓存最近一次，所以晚建的设置页也能拿到；但若 Glavo 比 UI
先起，开机前就插着的 U 盘要等一次插拔才认得到。要彻底解决得加一条「查询 U 盘当前态」
的请求（走 COMMON.md 四步）。

## UI 侧没做的（不在本次范围）

- **熄屏时间**是纯 UI 占位，选项只存在内存里，不落盘、也不真的熄屏。要真做得先定
  熄屏策略（空闲计时 + 背光置 0 或 sysfs blank + 触摸唤醒）。
- **录像存储位置 / 导出日志**只做了界面：选择结果落盘（`ui_prefs`），但录像仍写内置存储，
  Export 只提示 Coming soon。见 app/ui/UI.md「设置页布局」的只做界面清单。
- 旧版的「文件管理」两行（Album / Models，只画不接）随新稿删了。

# 选文件页 & 打印流程改造 — Agent 存档

> **用途**：新开 Agent 时先读本文，再读 `common/middleware/message_bus/msg_types.hpp` 与 `app/ui/lib/core/src/control.cpp`。  
> **索引**：[ARCHITECTURE.md](./ARCHITECTURE.md) · 契约流程 [common/README.md](../common/README.md) · UI [app/ui/UI.md](../app/ui/UI.md) · Glavo [app/glavo/GLAVO.md](../app/glavo/GLAVO.md)  
> **存档时间**：2026-06-25（初版）；**2026-06-25 增补 §12–§14**；**2026-06-25 v3**（基板弹窗二选一、PM2.5 分支、USB/config.json、路径定稿）  
> 基于多轮讨论 + 设计稿泳道图 + `doc/解答.txt` 取向

---

## 1. 背景与目标

用户更新了**打印前文件系统选择** UI（page / components / assets / `control.cpp`），需要与中间契约层、`ui_msg_proxy`、Glavo 后端对齐。

设计稿泳道图要点：

- **UI**：选文件 → Start Print → Prepare → 基板弹窗（用户选通路）→ 关门 → 打印
- **core/control**：状态机 + 调 Proxy；**基板是否放入无传感器**，由弹窗让用户自选通路
- **Glavo**：USB 复制、DirOpen、Prepare 解析、Start 下发

当前实现已**简化**为：

- 选文件页 = PageManager 隐藏页 `user_files`（Local/USB Tab + 扁平切片文件列表，**无文件夹层级**）
- 「Send」拆成多条消息：进页查供应度/目录；打印链路见 **§12**（Prepare 解析与 Start 下发分离）
- 打印配置 = 选中项 `g_active_file` + Tab 来源（Local/USB），**无独立 PrintConfig 结构体**

---

## 2. 已确认的设计决策（必读）

来源：`doc/解答.txt` + 多轮讨论 + 设计稿弹窗。**后续实现须遵守，勿自行加回已否决方案。**

| 决策 | 说明 |
|------|------|
| **不为 Local/USB 单独开 MsgType** | 不在 `msg_types.hpp` 增加 `FsSource` 等切源契约枚举；UI 本地 `FileTab { Local, Usb }` |
| **Tab 切源用 DirOpen 路径** | `Fs_DirOpen_Req` payload 带 **path 字符串**（空则 `/home/Gcode`） |
| **路径常量** | Local：`/home/Gcode`；USB：`/media/udisk0` |
| **Local/USB 双 index 域** | 两套列表各自 0-based index；Prepare/Start 均带 **`file_source` + `file_index`** |
| **目录列表过滤** | Local 与 USB **均只显示** `ParserRegistry::match_filename` 已注册后缀（现 `.gcode`、`.nc` 等） |
| **文件来源在 Prepare/Start 告知 Glavo** | Prepare 开解析；Start 仅 `begin_sending`；见 §12 |
| **USB 复制/预处理归 Glavo** | 在 **`UI_Print_Prepare_Req`** 完成；**不向 UI 同步拷贝进度**；UI 可显示无进度条的加载态 |
| **USB 同名覆盖** | 从 U 盘复制到 `/home/Gcode` 时，**同名文件默认覆盖** |
| **USB 复制后更新 config.json** | 相当于 Local 目录新增成员；Glavo 写 manifest；Local Tab DirOpen 时合并展示 |
| **解析与下发分离** | Prepare：`open_file` + `start_parse`（**不** `begin_sending`）；关门 ok 后 Print Start 才下发 |
| **基板弹窗 = 用户二选一，无检测** | **无**基板传感器、**无** `SupplyStatusPayload.plate_present`；泳道图「基板是否放入」= 设计稿弹窗，由用户选通路 |
| **基板弹窗时机** | **`APP_Print_Prepare_Done` 之后**弹出 D1；**不是** Start Print 后立刻 D1（现码待改） |
| **基板弹窗 UI 已定稿** | **不修改布局**；沿用 `show_insert_prompt`：滑动 SWIPE TO OPEN +「已放入打印版，直接前往打印」+ X |
| **滑动开门链路** | SWIPE TO OPEN → **PM2.5**（不安全则净化）→ 开门 → 开门 Rsp **立刻** `UI_CLOSE_DOOR_Req` |
| **直接前往打印链路** | 用户声明基板已在舱内 → **跳过 PM2.5、跳过开门** → **立刻** `UI_CLOSE_DOOR_Req` |
| **「用户放基板」** | 弹窗文案/状态说明 only；**不**阻塞 IPC、**不**等传感器 |
| **基板弹窗 X → 退出解析** | `UI_Print_Parse_Abort_Req`；Glavo：`stop_parse()` + 清 RingBuffer；**不要** TCP reset |
| **供应度独立查询** | 进文件页发 `UI_Supply_Status_Req`（耗材/滤芯）；与基板通路**无关**；传感器未接时 stub |
| **去掉 FileInfo 通道** | 详情用 `Fs_DirOpen_Rsp` 的 `FileEntryPayload`（含 `file_size`） |
| **USB 热插拔暂不实现** | 删 `register_storage_changed`、Storage Ntf、download MsgType |
| **PM2.5/净化** | **保留**；后续接真硬件；现 Glavo stub |
| **最后二次确认弹窗** | **暂不做** |

---

## 3. 当前实现状态（快照）

### 3.1 已完成（UI 侧为主）

| 项 | 位置 | 状态 |
|----|------|------|
| 选文件页 UI | `control.cpp` `show_file_select()`、`page_user_files_*` | ✅ Local/USB Tab、列表、详情、供应度条 |
| PageManager 注册 | `ui_app.cpp` 隐藏页 `user_files` | ✅ |
| 打印状态机 D1→D4→PRINTING | `control.cpp` `enter_state()` | ✅ 枚举可复用，**时序待重排** |
| 供应度契约 | `msg_types.hpp` `UI_Supply_Status_*` + `SupplyStatusPayload` | ✅ |
| 供应度 Proxy | `ui_msg_proxy` send/register + init 订阅 Rsp | ✅ |
| 供应度 UI 回调 | `control.cpp` `register_supply_status` → `refresh_supply_bars()` | ✅ |
| 进页拉供应度 | `page_user_files_refresh()` 调 `send_supply_status()` | ✅（顺序见 §5.1） |
| 基板弹窗 UI | `show_insert_prompt()` | ✅ 布局已定稿；文案可对齐设计稿中文 |

### 3.2 未完成 / 与决策冲突（待改）

| 项 | 问题 |
|----|------|
| `middleware::FsSource` | `control.cpp` 使用，**契约中不存在** → 改为 UI 本地 `FileTab` |
| `send_fs_dir_open(g_fs_source)` | Proxy 无 path 参数；Tab 切换无法传 `/home/Gcode` / `/media/udisk0` |
| download / storage / file_info | `control.cpp` 死代码 → **删除** |
| `PrintStartReqPayload` | 仅 `file_index`，**缺 `file_source`** |
| `FileEntryPayload` | 无 `file_size` |
| Glavo `handle_supply_status_req` | **未实现** |
| Glavo `handle_print_prepare` / Parse Abort | **未实现** |
| Glavo `handle_print_start` | 一次性 open+parse+begin_sending；未读 `file_source` |
| Glavo DirOpen | 单一 `current_entries_`；需 **Local/USB 分桶** + `file_size` + config.json 合并 |
| Glavo USB Prepare | 未复制到 `/home/Gcode`、未写 config.json |
| `control.cpp` 状态机 | Start 后**立刻** D1（应在 Prepare Done 后）；D1 X 无 Parse Abort |
| `UI_Print_Prepare_*` / `UI_Print_Parse_Abort_*` | 契约与 Proxy **未建** |
| `config.json` | manifest 读写 **未实现**（USB 复制路径 **纳入本期**） |
| `common/README.md` | 待同步 |

---

## 4. 目标契约（实现后应长这样）

### 4.1 保留并使用的消息

| MsgType | 方向 | Payload | UI API | Glavo |
|---------|------|---------|--------|-------|
| `Fs_DirOpen_Req` | UI→Glavo | Empty 或 **path 字符串** | `send_fs_dir_open(path?)` | `handle_dir_open` |
| `Fs_DirOpen_Rsp` | Glavo→UI | `FsDirHeader` + `FileEntryPayload[]` | `register_fs_dir` | 扫目录 + stat + **读 config.json（Local）** |
| `UI_Supply_Status_Req` | UI→Glavo | Empty | `send_supply_status()` | stub |
| `APP_Supply_Status_Rsp` | Glavo→UI | `SupplyStatusPayload` | `register_supply_status` | publish_async |
| `UI_Print_Prepare_Req` | UI→Glavo | `PrintPrepareReqPayload` | `send_print_prepare()` | USB 复制+写 manifest → open+parse → `APP_Print_Prepare_Done` |
| `APP_Print_Prepare_Done` | Glavo→UI | `{ int8_t ok; }` 建议 | `register_print_prepare_done` | ok=0 时 UI toast + IDLE |
| `UI_Print_Parse_Abort_Req` | UI→Glavo | Empty | `send_print_parse_abort()` | stop_parse + clear rb（基板 X） |
| `UI_Print_Start_Req` | UI→Glavo | `PrintStartReqPayload` | `send_print_start(index, source)` | **仅** begin_sending + 录像 |
| `APP_Print_Progress_Ntf` | Glavo→UI | `PrintProgressPayload` | `register_print_progress` | 已有 |
| 门控 / PM2.5 / 终止等 | — | — | 已有 | 已有 |

### 4.2 建议扩展的 Payload

```cpp
enum class PrintFileSource : uint8_t {
    Local = 0,
    Usb   = 1,
};

struct PrintPrepareReqPayload {
    uint32_t        file_index;
    PrintFileSource file_source;
    uint8_t         reserved[3];
};

struct PrintStartReqPayload {
    uint32_t        file_index;
    PrintFileSource file_source;
    uint8_t         reserved[3];
};

struct PrintPrepareDonePayload {
    int8_t ok;   /**< 1=成功（含 USB 复制+解析已启）；0=失败 */
};

struct FileEntryPayload {
    uint32_t index;
    char     name[64];
    char     path[128];
    uint32_t duration_min;
    float    material_g;
    uint32_t file_size;
};
```

### 4.3 路径与目录

| 用途 | 路径 |
|------|------|
| Local 缓存 / USB 复制目标 | `/home/Gcode` |
| USB 挂载 | `/media/udisk0` |
| Local manifest | `/home/Gcode/config.json` |

- **每次点击 Local Tab**（或 `page_user_files_refresh` 在 Local 下）→ `Fs_DirOpen_Req("/home/Gcode")` **刷新列表**
- USB 上 Start Print → 复制并写 config.json → 打印结束/终止后用户切 Local Tab → **应能看到新文件**

### 4.4 config.json（Local manifest，本期 USB 路径必做）

Glavo 维护；UI **不直接读文件**。

建议字段（与先前讨论一致，实现时可微调）：

```json
{
  "files": [
    {
      "name": "part.gcode",
      "file_size": 123456,
      "duration_sec": 0,
      "material_g": 0.0,
      "line_count": 0,
      "mtime": 1710000000
    }
  ]
}
```

| 时机 | Glavo 动作 |
|------|------------|
| USB Prepare 复制成功 | 写入/更新 `/home/Gcode/config.json` 对应项（同名覆盖） |
| Local DirOpen | `opendir` + `stat` 与 manifest **合并**（manifest 补 duration/material 等；磁盘为准删 stale） |
| USB DirOpen | 仅扫 `/media/udisk0`，**不读** Local 的 config.json |

`duration_min` / `material_g` 若 manifest 为 0，后续可接解析估算（P1）。

### 4.5 建议删除的消息与类型

| 删除项 | 范围 |
|--------|------|
| `UI_FileInfo_Req` / `APP_FileInfo_Rsp` / `FileInfoPayload` | 契约、Proxy、Glavo、`rs_msg_format.cpp`、`common/README.md` |
| download / storage 相关 | `control.cpp`；**不要**加到 Proxy |
| `middleware::FsSource` | 改为 UI 本地 `FileTab` |

---

## 5. 端到端流程（目标态）

### 5.1 进选文件页 `user_files`

```
page_user_files_refresh() [on_enter / Tab 切换]
  ① send_supply_status()
  ② send_fs_dir_open(path)     // Local=/home/Gcode, USB=/media/udisk0
  → APP_Supply_Status_Rsp → refresh_supply_bars()
  → Fs_DirOpen_Rsp → rebuild_file_grid()
```

用户选列表行 → `g_active_file`（本机：耗时/耗材；USB：`file_size`）。

### 5.2 点 Start Print — **目标态（§12）**

> **现码仍走 §5.3**；实现 P0 后切换为本节。

```
Start Print
  → UI_Print_Prepare_Req（index + source）
  → Glavo：
       Local: open_file(/home/Gcode/…) + start_parse
       USB:   copy(/media/udisk0/… → /home/Gcode/…, 同名覆盖)
              → 更新 config.json
              → open_file + start_parse
       （均不 begin_sending；拷贝进度不同步 UI）
  → APP_Print_Prepare_Done（ok=1）
  → D1 基板弹窗（设计稿；用户二选一，无检测）
       ├─ X → UI_Print_Parse_Abort_Req → stop_parse + clear rb → IDLE
       ├─ SWIPE TO OPEN → D2 PM2.5 → [净化] → UI_OpenDoor_Req
       │       → APP_OpenDoor_Rsp ok → **立刻** UI_CLOSE_DOOR_Req（D4）
       └─「已放入打印版，直接前往打印」→ **跳过 PM2.5/开门** → **立刻** UI_CLOSE_DOOR_Req（D4）
  → APP_CLOSE_DOOR_Rsp ok
  → PRINTING
       → switch_to("user_home") + 相机预览
       → UI_Print_Start_Req → Glavo begin_sending + 录像 Ntf
  → APP_Print_Progress_Ntf … → IDLE
```

Prepare 失败（`ok=0`）：toast + 回 IDLE，**不**进 D1。

### 5.3 点 Start Print — **当前态（现码，待改）**

```
Local：enter_state(D1) 立刻出基板弹窗          ← 应在 Prepare Done 后
USB：  加载弹窗 + send_file_download（未实现） ← 应改为 Glavo Prepare

D1 → SWIPE → D2(PM2.5) → [净化] → 开门 → D4(关门)   ← 开门后立刻 D4，与目标一致
  或「Plate inserted…」→ 跳过 PM2.5/开门 → D4        ← 与目标一致

D4 关门 ok → PRINTING
  → send_print_start → Glavo 一次性 open+parse+begin_sending   ← 待拆分
D1 X → enter_state(IDLE)，无 IPC                               ← 待加 Parse Abort
```

### 5.4 状态机（目标）

```
IDLE
  → Start Print → PREPARE（等 Done）
  → D1 基板弹窗
       → SWIPE → D2(PM2.5) → [D2_PURIFY] → OPEN_DOOR → D4
       → 直接前往 → D4
       → X → ABORT → IDLE
  → D4 关门 ok → PRINTING → IDLE
```

复用 `CONTROL_STATE_D1 / D2 / D2_PURIFYING / D4`；新增 PREPARE 等待态（或标志位）。

---

## 6. 分文件改动清单（Agent 执行顺序）

### P0 — 契约 + 选文件页 + config.json 基础

- [ ] **`msg_types.hpp`**：`PrintFileSource`、`PrintPrepareReqPayload`、`PrintStartReqPayload` 扩展、`PrintPrepareDonePayload`、`FileEntryPayload.file_size`；删 FileInfo；新增 Prepare/ParseAbort MsgType
- [ ] **`common/README.md`** + **`rs_msg_format.cpp`** 同步
- [ ] **`ui_msg_proxy.*`**：`send_fs_dir_open(path)`、`send_print_prepare` / `register_print_prepare_done` / `send_print_parse_abort`、`send_print_start(index, source)`；删 FileInfo
- [ ] **`control.cpp`（选文件）**：`FileTab`、path 常量、删 download/storage/file_info；Tab 切换刷新 DirOpen
- [ ] **`glavo_service.*`（DirOpen + manifest）**：
  - `local_entries_` / `usb_entries_` 分桶
  - Local DirOpen：扫盘 + 读 `/home/Gcode/config.json` 合并 + `file_size`
  - USB DirOpen：`/media/udisk0` + `file_size`
  - `handle_supply_status_req` stub
  - 删 `handle_file_info_req`

### P0 — Prepare / 舱体状态机 / Start 收窄

- [ ] **`glavo_service.*`**：
  - `handle_print_prepare`：USB copy（同名覆盖）→ 写 config.json → open+parse → `APP_Print_Prepare_Done`
  - `handle_print_parse_abort`：stop_parse + clear rb（不 terminate）
  - `handle_print_start`：**仅** begin_sending + 录像（解析已在跑）
- [ ] **`control.cpp` 状态机**：
  - Start → Prepare（简单 loading，无拷贝进度）→ Done → **D1**
  - D1 X → `send_print_parse_abort()`
  - 滑动链路：PM2.5 → 开门 → **开门 Rsp 立刻 D4**
  - 直接链路：**立刻 D4**
  - D4 ok → PRINTING → `send_print_start`
- [ ] **暂不做**：最后二次确认弹窗；USB 拷贝进度 Ntf

### P1 — 体验与文档

- [ ] manifest 中 duration/material 接解析或估算
- [ ] 弹窗文案对齐设计稿中文（**不改布局**）
- [ ] 归档 **`doc/compare.cpp`**；更新 **`GLAVO.md`**

### 不做

- USB 热插拔；文件夹层级；FileInfo；download MsgType；基板传感器 / `plate_present`

---

## 7. 关键文件路径速查

| 模块 | 路径 |
|------|------|
| 契约 | `common/middleware/message_bus/msg_types.hpp` |
| UI Proxy | `app/ui/lib/core/inc/ui_msg_proxy.hpp`、`ui_msg_proxy.cpp` |
| 打印编排 + 选文件页 | `app/ui/lib/core/src/control.cpp`、`control.hpp` |
| Glavo IPC | `app/glavo/lib/glavo_service.cpp` |
| 解析后缀注册 | `app/parser/` `ParserRegistry::match_filename` |
| Local manifest | `/home/Gcode/config.json`（Glavo 读写） |

---

## 8. 架构红线（勿破）

- UI 走 `UiMsgProxy`；禁止链 `app/glavo/lib`
- Glavo 回复 UI **`publish_async`**
- 改 MsgType/Payload 走 `common/README.md` 四步

---

## 9. Glavo 供应度 stub 参考

```cpp
void GlavoService::handle_supply_status_req(const middleware::Message& msg) {
    middleware::SupplyStatusPayload p{};
    p.material_pct = 48.0f;
    p.filter_pct   = 48.0f;
    p.material_ok  = 1;
    p.filter_ok    = 1;
    // publish_async APP_Supply_Status_Rsp → msg.sender_id
}
```

---

## 10. 与旧讨论的对应关系

| 讨论点 | 结论 |
|--------|------|
| 基板是否放入 | **弹窗用户自选**，无 UI/传感器检测；**不加** `plate_present` |
| PM2.5 | **仅** SWIPE TO OPEN 链路；直接前往打印 **跳过** |
| 开门后关门检测 | 滑动链路：开门 Rsp **立刻** CLOSE_DOOR；**不是**开门后再等用户点「直接开始」 |
| USB 复制 | 同名覆盖；**不同步进度**；写 config.json |
| Local 见新文件 | USB 打印后切 Local Tab → DirOpen 刷新 → 列表含拷贝项 |
| 解析 vs 下发 | Prepare 后 parse；Start 后 begin_sending |
| 基板 X | Parse Abort；不是 Terminate |
| config.json | USB 路径 **本期必做**；P1 补全 duration/material 估算 |

---

## 11. 验收建议

### 选文件页

1. Local/USB Tab：DirOpen 路径正确；仅显示 `.gcode`/`.nc` 等已注册后缀
2. 供应度 stub 更新；无 FileInfo / FsSource / download

### Prepare + 舱体

3. Start → Prepare Done 后 Glavo **已在 parse**，**未** begin_sending
4. Prepare Done **之后**才出现 D1 弹窗
5. SWIPE：PM2.5 → 开门 → 开门 Rsp 后 **立刻** D4
6. 直接前往：无 PM2.5/开门，**立刻** D4
7. D1 X → Parse Abort → rb 清空 → IDLE
8. D4 ok → Print Start **仅** begin_sending + UI 预览

### USB + manifest

9. USB Prepare：文件出现在 `/home/Gcode`，config.json 有记录
10. 打印结束/终止后切 Local Tab：列表可见该文件

---

## 12. 舱体与解析流程 — 泳道图 v2（设计目标）

> **最后二次确认弹窗：暂不做。**  
> **基板弹窗 UI 已定稿，实现时不改布局。**

### 12.1 目标时序

```
用户点 Start Print（g_active_file + Tab 来源）
    │
    ▼
┌─────────────────────────────────────┐
│ Glavo Prepare                        │
│  Local: open + parse                 │
│  USB:   copy→/home/Gcode(覆盖)       │
│         → 更新 config.json           │
│         → open + parse               │
│  不 begin_sending                    │
└─────────────────────────────────────┘
    │ APP_Print_Prepare_Done ok
    ▼
┌─────────────────────────────────────┐
│ D1 基板弹窗（用户二选一，无检测）     │
│  · X → Parse Abort                   │
│  · SWIPE TO OPEN → PM2.5→[净化]→开门 │
│  · 已放入…直接前往打印               │
└─────────────────────────────────────┘
    │
    ├─ SWIPE 链路：OpenDoor Rsp ok ──► 立刻 CLOSE_DOOR（D4）
    └─ 直接链路：──────────────────► 立刻 CLOSE_DOOR（D4）
    │ CloseDoor Rsp ok
    ▼
┌─────────────────────────────────────┐
│ PRINTING                             │
│  UI：home + 相机预览                 │
│  UI_Print_Start_Req → begin_sending  │
└─────────────────────────────────────┘
```

### 12.2 基板弹窗 ↔ 设计稿控件

| 设计稿 | 用户含义 | 软件 |
|--------|----------|------|
| SWIPE TO OPEN | 需开门放基板 | PM2.5 → 开门 → 开门 Rsp **立刻**关门检测 |
| 已放入打印版，直接前往打印 | 基板已在舱内 | 跳过 PM2.5/开门 → **立刻**关门检测 |
| X | 取消 | Parse Abort + IDLE |
| 副文案「打印前请先放入…」 | 提示 only | 不阻塞 |

### 12.3 解析与下发的职责分离

| 阶段 | Glavo | McuLink |
|------|-------|---------|
| Prepare | copy(Usb) + manifest + `open_file` + `start_parse` | 不 `begin_sending` |
| Parse Abort | `stop_parse()` + clear rb | 不 terminate |
| Print Start | `begin_sending()` + 录像 | 从 rb 下发 |

### 12.4 门控触发点

| 消息 | UI 触发点 |
|------|-----------|
| `UI_OpenDoor_Req` | D1 **SWIPE 链路**，PM2.5/[净化] 通过之后 |
| `UI_CLOSE_DOOR_Req` | ① 开门 Rsp ok（滑动链路，**立刻**）② D1「直接前往」（**立刻**） |

### 12.5 Parse Abort（基板 X）

```cpp
void GlavoService::handle_print_parse_abort(const middleware::Message& msg) {
    (void)msg;
    if (print_running_.load()) return;  /* 打印中应走 Terminate */
    stop_print_job();  /* stop_parse + clear rb；不 request_terminate */
}
```

### 12.6 新增契约摘要

| MsgType | Payload | 说明 |
|---------|---------|------|
| `UI_Print_Prepare_Req` | `PrintPrepareReqPayload` | index + source |
| `APP_Print_Prepare_Done` | `PrintPrepareDonePayload` 建议 | ok=0 失败 |
| `UI_Print_Parse_Abort_Req` | Empty | 基板 X |
| `UI_Print_Start_Req` | `PrintStartReqPayload` | 仅下发 |

---

## 13. 现码 vs 目标差异表

| 环节 | 目标 | 现码 | 符合 |
|------|------|------|------|
| Prepare + parse | Start 后先 Prepare | 关门后才 open+parse | ❌ |
| D1 时机 | Prepare Done **后** | Start 后**立刻** D1 | ❌ |
| 滑动：开门→关门 | 开门 Rsp **立刻** D4 | 同 | ✅ |
| 直接前往 | 跳过 PM2.5/开门 → D4 | 同 | ✅ |
| D1 X | Parse Abort | 仅 IDLE | ❌ |
| Print Start | 仅 begin_sending | open+parse+send 一次 | ❌ |
| USB copy + manifest | Prepare 内 | 未实现 | ❌ |
| DirOpen 双桶 + path | Local/USB 独立 index | 单 `current_entries_` | ❌ |
| 弹窗布局 | 设计稿已定 | 结构已有，时序错 | ⚠️ |

### 13.1 现码锚点

| 行为 | 符号 |
|------|------|
| Start 立刻 D1 | `on_detail_start_print` |
| 滑动 → PM2.5 → 开门 → 立刻 D4 | `on_insert_unlocked` → `pf_start_open_door_then_close_check` |
| 直接 → D4 | `on_insert_shortcut` |
| D1 X 无 IPC | `on_insert_close` |
| 一次性 print_start | `glavo_service.cpp` `handle_print_start` |

---

## 14. Glavo Prepare 参考（USB 路径）

```cpp
void GlavoService::handle_print_prepare(const middleware::Message& msg) {
    /* 1. 按 file_source + index 取 usb_entries_[i] 全路径 */
    /* 2. copy 到 /home/Gcode/<basename>，同名覆盖 */
    /* 3. upsert /home/Gcode/config.json */
    /* 4. open_file(dest) + start_parse() */
    /* 5. publish_async APP_Print_Prepare_Done { ok=1 } */
}
```

---

*本文档随实现推进可更新「§3 当前状态」勾选进度；重大决策变更请同步修改 §2。*

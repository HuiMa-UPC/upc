# ui/ — LVGL 界面进程

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_ui`** · 库 **`ui::port`**

## 职责

| 子目录 | 说明 |
|--------|------|
| `lib/core/` | PageManager、`UiMsgProxy`、`control`、摄像头预览 |
| `lib/pages/` | 各 Tab 布局（`page_user_*`） |
| `lib/components/` | 公用 LVGL 控件 |
| `lib/assets/` | 图片、字库 |

## 红线

- **禁止**链接 `glavo/lib`、`parser/`
- **禁止** pages 直接 `MsgPort::subscribe` / `publish` → 走 **Proxy**
- **LVGL 仅 UI 线程**；后端回调经 `publish_async` + 主循环 `process()`
- 页面注册只在 **`ui_app.cpp`**，不在 `main.cpp` include 各页面头

## 主循环（`main.cpp`）

```cpp
connect_ipc_with_retry();  // hub + add_ipc_peer
platform_init();
ui::app_init(lv_scr_act());

while (!g_shutdown_requested) {
    const uint32_t delay = lv_timer_handler();
    port.process(delay > 0 ? (int)delay : 1);
    port.process(0);   // 同帧 Req/Rsp
    upc_sys_monitor_tick();
}
```

## PageManager 约定

- `Page::name()` 返回唯一 ID（如 `"demo"`），与 `switch_to("demo")` **完全一致**
- `register_page` 只登记；`switch_to` 首次触发 `on_create`
- 离页时在 `on_exit` 里 `register_*(nullptr)` 取消 Proxy 回调

## UiMsgProxy（Glavo）

| 方法 | 方向 |
|------|------|
| `send_*()` | UI → `APP_GLAVO` |
| `register_*()` | Glavo → UI 回调 |

`Subscription` 必须保存在 `subs_` 等成员中。其它后端用独立 `XxxMsgProxy`，不要堆进 UiMsgProxy。

## 多后端 IPC

- UI = **Hub**；`add_ipc_peer(ModuleId, path, required)`
- 单播：`target_id = APP_*`；广播：`target_id = 0`
- 启动顺序见 `scripts/rongsu.sh`
- 细节：[common/ipc/IPC.md](../common/ipc/IPC.md)

当前 peer：`APP_GLAVO`（required）、`APP_CAMERA`（可选，预览）。

## CMake

`ui/CMakeLists.txt` GLOB `lib/*/src`；新页面 `.cpp` 放入 `pages/src/` 即可。

## 常见错误

| 现象 | 处理 |
|------|------|
| 收不到 Glavo 回复 | 主循环是否 `process()` |
| 切页后仍收旧回调 | `on_exit` 清空 register |
| IPC 发错后端 | 检查 `target_id` |

## 相关文档

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [common/ipc/IPC.md](../common/ipc/IPC.md) · [contract/README.md](../common/contract/README.md)

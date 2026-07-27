# app/ui/ — LVGL 界面进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_ui`** · 库 **`ui::port`**

## 职责

| 子目录 | 说明 |
|--------|------|
| `lib/core/` | PageManager、`UiMsgProxy`、`control`、摄像头预览 |
| `lib/pages/` | 各 Tab 布局（`page_user_*`） |
| `lib/components/` | 公用 LVGL 控件 |
| `lib/assets/` | 图片、字库 |

## 红线

- **禁止**链接 `app/glavo/lib`、`app/parser/`
- **禁止** pages 直接 `MsgPort::subscribe` / `publish` → 走 **Proxy**
- **LVGL 仅 UI 线程**；后端回调经 `publish_async` + 主循环 `process()`
- 页面注册只在 **`ui_app.cpp`**，不在 `main.cpp` include 各页面头

## 主循环（`main.cpp`）

IPC 收包、`deliver_local`（Proxy 回调）与 LVGL 渲染**同一线程**；渲染前先泵 IPC，避免大画面刷新拖慢 Glavo 控制消息。

```cpp
connect_ipc_with_retry();  // hub + add_ipc_peer
platform_init();
ui::app_init(lv_scr_act());

while (!g_shutdown_requested) {
    port.process(0);                        // 渲染前先 drain IPC
    const uint32_t delay = lv_timer_handler();
    int wait_ms = delay > 0 ? (int)delay : 1;
    wait_ms = std::min(wait_ms, 8);         // 限制 blocked poll，避免拖死 IPC
    port.process(wait_ms);
    port.process(0);                        // 同帧 Req/Rsp
    upc_sys_monitor_tick();
}
```

后端→后端（如 Glavo→Camera 打印录像）由 Hub 在 `process()` 内 **`IPC_FWD` 同步转发**，不经过 UiMsgProxy。预览像素走 dma fd 旁路 + SHM 元数据环，不走 MsgPort payload。

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

### 选文件页目录数据

| Tab | 数据源 |
|-----|--------|
| **Local** | UI 直接读 `/home/Gcode/config.json`（`common/fs_manifest/gcode_manifest.*`），**不发** `Fs_DirOpen_Req` |
| **USB** | `send_fs_dir_open(Usb)` → Glavo 扫盘 → `Fs_DirOpen_Rsp` |

删除 Local / Prepare / Start 仍走 IPC。

## 页面（`lib/pages/`）

| Page ID | 文件 | 说明 |
|---------|------|------|
| `user_home` | `page_user_home.*` | 首页：环境、打印模式、预览、打印结果页 |
| `user_history` | `page_user_history.*` | 打印历史：读 manifest + 卡片 + 缩略图(TJPGD) + 左滑删除 |
| `user_control` | `page_user_control.*` | skeleton 占位 |
| `user_settings` | `page_user_settings.*` | 设置列表 + 开发者密码弹窗 |
| `user_message` | `page_user_message.*` | skeleton 占位 |
| `user_timelapse` | `page_user_timelapse.*` | 延时摄影播放器（隐藏页，历史缩略图进入；全屏 `lv_layer_top`） |

注册仅在 `ui_app.cpp`；打印流程走 `control_*`，pages 不直接订阅 MsgBus。详见 `lib/pages/README.md`。

### 打印结果页（PRINT COMPLETE / PRINT TERMINATED）

不是独立 Page，而是 `page_user_home` 打印中浮层 `g_print_area` **内部**的第三层
（`g_result_layer`），与打印中底条 `g_print_bottom` 互斥。摄像头背景与
`CameraPreviewPlayer` 完全沿用打印中那一套，中途不停流，避免黑屏闪烁。

时序：

```
Glavo Print_Terminate_Done / 进度 100% → control.cpp finish_printing()
  → enter_state(CONTROL_STATE_PRINT_DONE)   // 只停看门狗，预览保活，界面维持打印中原样
  → （Camera 后台把 PNG 序列编成 timelapse.mp4 并 upsert manifest，可能数秒到数分钟）
  → APP_Print_Video_Manifest_Ntf (Camera→UI)
  → control.cpp on_print_video_manifest()：按 frames_dir 主键读 /home/video/manifest.json
  → page_user_home_enter_print_result()     // 换成结果页浮层
```

- **数据源**：`has_entry=1` 时以 manifest 条目为权威（`status` / `duration_sec` / `material_g`）；
  `has_entry=0`（0 帧未落盘）时用 payload 的 `aborted` 判完成/终止，耗时耗材显示 `--`。
- **两个出口**：右上圆钮 `control_print_result_dismiss()` 直接回 IDLE；
  右下 `SWIPE TO COLLECT` → `control_print_result_collect()` 回 IDLE 后走开门流程。
  两者都经 IDLE 分支的 `page_user_home_exit_printing_mode()` 统一收浮层 + 停预览。
- **PRINT_DONE 期间** `control_is_printing()` 返回 true → PageManager 挡住 `user_files`，
  取件前不能开新任务。
- 中间的 Purify 圆钮是 UI 占位，后端净化流程尚未实现。

### WiFi 传输弹窗（PIN 配对 / 文件接收）

`lib/components/ui_xfr_dialog.*`，对齐 Figma 3859:9545 与 3859:9810。两者共用
`ui_modal_dialog` 外壳，只在建窗时就地覆盖两处（卡内行距 `USER_PAD_SM`、遮罩
`LV_OPA_50`），不动 `USER_DLG_*` 宏以免连带改掉打印流程那批弹窗。

挂 `lv_layer_top()` 而不是 `control` 的 `g_overlay`：配对/传输是网络事件，任何
时刻都可能到，而 `g_overlay` 在 IDLE 态是隐藏的。按设计打印中也照弹。

| 弹窗 | 触发 | 状态 |
|------|------|------|
| PIN 配对码 | `XFR_Pair_Pin_Ntf` → `control.cpp wire_msg_proxy()` | 已接线 |
| 文件接收进度 | `XFR_Transfer_Progress_Ntf` → `control.cpp wire_msg_proxy()` | 已接线 |

接收弹窗由 XFR 进度推送驱动：`Uploading/Slicing` 状态自动开窗并更新进度条；
`Finished/Error/Idle` 自动收窗。`XFR_File_Received_Ntf` 仍保留原有 toast 和
本地文件列表刷新逻辑。

两处已知遗留（缺契约，非实现疏漏）：

- **取消按钮只关 UI**。UI→XFR 方向目前一条消息都没有，点 `Cancel Pairing` 之后
  后端配对会话仍然存活，手机那时候依然能配对成功。
- **PIN 弹窗只能手动关**。`XfrPairPinPayload.expires_s` 没有使用，PIN 过期不会自动
  收窗；弹窗遮罩会吞掉全屏点击，无人处理时会一直挡着（含打印中）。

文案统一用英文：CJK 子集字库 `assets/src/lv_font_cjk_data.c` 只覆盖调试测试页用字
（403 字），缺稿上的「备 / 码 / 消」，且只有单一 18px 字号。

## CameraMsgProxy / CameraPreviewPlayer（Camera）

| 组件 | 文件 | 职责 |
|------|------|------|
| `CameraMsgProxy` | `camera_msg_proxy.*` | `UI_Camera_Stream_*` / Snapshot IPC 封装 |
| `CameraPreviewPlayer` | `camera_preview.*` | dma fd 元数据环消费、NV12→BGRX、LVGL 显示 |
| `RgaDmaBuf` | `rga_dma_buf.*` | 封装 `common/ipc/dma_heap_buffer` 的 dma32 heap 缓冲，供 RGA `importbuffer_fd` |

| 方法 | 方向 |
|------|------|
| `send_stream_start()` / `send_stream_stop()` | UI → `APP_CAMERA` |
| `register_stream_start_rsp()` | Camera → UI（分辨率、SHM 元数据环名、`dma_mode`） |
| `register_print_video_manifest()` | Camera → UI（manifest 已落盘，`control.cpp` 据此切打印结果页） |

### 预览数据路径

预览统一走 dma fd 旁路（`dma_mode=1`）：

```
rongsu_camera 经主 IPC（/tmp/upc_camera.sock）SCM_RIGHTS 附带 dma_buf fd 表
  → CameraPreviewPlayer::on_stream_start_rsp 调用 MsgPort::take_peer_dma_fds(APP_CAMERA)
  → SHM /upc_camera_preview 只读 slot 状态/seq/buffer_idx
  → 解码线程 acquire_latest_frame 得到 buffer_idx
  → [RGA] importbuffer_fd(src_fd) → imcvtcolor → slots_[3] BGRX
  → [SW 回退] mmap(src_fd) → nv12_to_bgrx_sw → slots_
  → lv_timer(33ms) 换 img_dsc_ 指针 → lv_image (XRGB8888)
```

`CameraStreamInfoPayload.dma_socket_path` 为 ABI 保留字段；fd 不经独立 socket 传递。

- **开流**：`page_user_home` 打印模式 → `CameraPreviewPlayer::start()` → IPC Start → `apply_stream_start_ui` 开 SHM 元数据环 + `take_peer_dma_fds` + `setup_frame_buffers` + 解码线程。
- **关流**：`stop()` → join 解码线程、`clear_frame_buffers()` 释放 DMA / 关闭 fd 表。
- **RGA 路径**：`setup_frame_buffers` 时 `RgaDmaBuf` 分配成功则 `rga_path=1`；失败退化为 `std::vector` 软件转换。无进程级预热，首帧到达即转换。
- **DMA 约束**：RK3568 RGA2 仅支持 4G 以下物理地址；源 fd 必须来自 V4L2 / dma-heap（dma32），UI 直接用 `importbuffer_fd`。
- **Stride 对齐**：RGA 源 stride 使用后端传来的实际 `stride_y`，不再假设 `stride_y == width`。
- **Cache 同步**：RGA 读源前 `DMA_BUF_IOCTL_SYNC(READ)`，写目的后 `DMA_BUF_IOCTL_SYNC(WRITE)`；目的缓冲优先 uncached。
- **线程**：解码在独立线程；`on_stream_start_rsp` 与换帧须 `lv_async_call` / `lv_timer` 回到 UI 线程。
- **CMake**：找到 `librga` 时 `-DUPC_UI_HAS_RGA=1`（当前源码仍硬依赖 RGA 头文件）。

## 打印历史 + 延时摄影播放器

| 组件 | 文件 | 职责 |
|------|------|------|
| `page_user_history` | `pages/page_user_history.*` | 读 `/home/video/manifest.json`（`common/fs_manifest/print_history_manifest`）→ 按 `start_unix` 倒序卡片列表；缩略图 `A:/home/video/{thumb}`（RTHM 解码，`LV_IMAGE_ALIGN_COVER`）；上下翻页；**左滑删除**（清除卡片默认 `GESTURE_BUBBLE` 使手势落到卡片）→ 删目录 + 重写 manifest + 只删单卡 |
| `page_user_timelapse` | `pages/page_user_timelapse.*` | 全屏播放页（建于 `lv_layer_top()` 覆盖侧边栏）：按序播放 `/home/video/{job_ts}/1.png...N.png`，顶部标题/时间、右上关闭、中央播放/暂停、底部进度条、点画面显隐控件 |

- **进入/退出**：历史页缩略图点击 → `page_user_timelapse_set_source(目录,mp4路径,标题,时间)` + `switch_to("user_timelapse")`；关闭 → `switch_to("user_history")`。
- **LVGL 线程规则**：序列播放由主线程 `lv_timer` 驱动换图。

### LVGL 配置依赖（`3rdparty/.../configs/rongsu_ui.defaults`）
- `LV_USE_TJPGD=1`：历史缩略图 baseline JPEG 解码
- `LV_USE_FS_POSIX=1` + `LV_FS_POSIX_LETTER='A'`：按路径加载 `/home/video` 图片（`"A:/home/video/x.png"` / `"A:/home/video/x.rthm"`）
- lvgl 目标加 `_DEFAULT_SOURCE`（`3rdparty/.../CMakeLists.txt`）：修 FS 驱动 `DT_DIR` 在 `_POSIX_C_SOURCE` 下不可见的编译错误
- 缩略图 `toast` 与播放器同建于 `lv_layer_top()`，全屏时提示可见

## 多后端 IPC

- UI = **Hub**；`add_ipc_peer(ModuleId, path, required)`
- 单播：`target_id = APP_*`；广播：`target_id = 0`
- 启动顺序见 `scripts/rongsu.sh`
- 细节：[common/COMMON.md](../../common/COMMON.md) § IPC

当前 peer：`APP_GLAVO`（required）、`APP_CAMERA`（optional，预览 + 打印录像转发）。

## CMake

`ui/CMakeLists.txt` GLOB `lib/*/src`；新页面 `.cpp` 放入 `pages/src/` 即可。

## 常见错误

| 现象 | 处理 |
|------|------|
| 收不到 Glavo 回复 | 主循环是否 `process()` |
| 切页后仍收旧回调 | `on_exit` 清空 register |
| IPC 发错后端 | 检查 `target_id` |

## 相关文档

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [common/COMMON.md](../../common/COMMON.md)

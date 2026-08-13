# 首层视觉检测开发指南（Visual 团队）

本文面向视觉算法开发同学，说明当前首层检测链路、需要对接的代码入口，以及联调注意事项。

## 1. 功能目标

- 在**首层**下位机上报 `PRINT_READY` 后，由 Glavo 触发 Visual 做一次检测。
- Visual 从 Camera 的 dma fd + SHM ring 拿到最新 NV12 原图，执行检测逻辑。
- Visual 回传检测结果：
  - `result=1`：首层通过，Glavo 继续出光打印。
  - `result=0`：首层不通过，Glavo 发送 `LAYER_DONE(layer_height_um=0)` 继续铺粉，不降基板。
- 仅首层生效；首层通过后，后续层走原流程，不再触发视觉检测。

## 2. 运行时开关（UI 调试页）

- 位置：Debug 页面 `Other` 页签中的 `1st Layer Visual` 开关。
- 默认值：关闭。
- 消息契约：
  - `UI_Debug_FirstLayer_Visual_Check_Set_Req`
  - `UI_Debug_FirstLayer_Visual_Check_Get_Req`
  - `GLAVO_Debug_FirstLayer_Visual_Check_Ntf`

关闭时：打印流程与历史逻辑一致，完全跳过首层视觉检测。

## 3. IPC 契约（Glavo ↔ Visual）

定义位置：`common/upc_contract.hpp`

- 请求：`GLAVO_Visual_FirstLayer_Check_Req`
  - 载荷：`VisualFirstLayerCheckReqPayload`
  - 字段：`layer_height_um`
- 响应：`APP_Visual_FirstLayer_Check_Rsp`
  - 载荷：`VisualFirstLayerCheckRspPayload`
  - 字段：`result`（1=通过，0=不通过）

## 4. 现有代码入口

### 4.1 Visual 侧

文件：`app/visual/lib/visual_service.cpp`

- 请求处理入口：`VisualService::on_first_layer_check_req()`
- 图像拷贝缓存：
  - `VisualService::worker_thread_fn()`
  - `VisualService::update_latest_snapshot()`
  - `VisualService::copy_latest_snapshot()`
- 算法预留入口（当前占位实现）：
  - `VisualService::process_first_layer_image_placeholder(...)`

当前占位行为：拿到最新 NV12 后直接返回通过（`true`），仅用于打通流程与联调。

### 4.2 Glavo 侧

文件：`app/glavo/lib/glavo_service.cpp`、`app/glavo/lib/mcu_link.cpp`

- Glavo 发送请求并等待响应：`GlavoService::request_first_layer_visual_check()`
- Glavo 接收 Visual 回包：`GlavoService::handle_visual_first_layer_check_rsp()`
- 首层门控逻辑（收到首层 `PRINT_READY` 后）：
  - `McuLink::link_process_serial_handshake()`

## 5. 算法接入建议

建议只改 Visual 进程，优先保持 IPC 契约与上层流程不变：

1. 在 `process_first_layer_image_placeholder(...)` 中接入真实检测逻辑。
2. 入参使用已拷贝的紧凑 NV12 数据（`width * height * 3 / 2`）。
3. 根据检测结果返回：
   - `true` → `result=1`
   - `false` → `result=0`
4. 如需更多调试信息，先通过日志输出，不要直接改动跨进程载荷字段。

## 6. 联调检查清单

- 开关关闭：
  - 打印应与旧流程一致，不触发视觉请求。
- 开关开启 + Visual 返回 1：
  - 首层等待后继续正常打印。
- 开关开启 + Visual 返回 0：
  - 不出光，且反复触发 `LAYER_DONE(0)` 继续铺粉，直到返回 1。
- 首层通过后：
  - 后续层不再触发视觉检测请求。

## 7. 注意事项

- Visual 图像数据来自 Camera 并行流，依赖 `VISUAL_Camera_Stream_Start_Req` 成功起流。
- 首层等待期间，Glavo 会等待 Visual 返回，不会继续推进打印流程。
- 若后续要扩展返回内容（置信度、缺陷类型等），请按 `common/COMMON.md` 的契约四步流程新增消息字段。

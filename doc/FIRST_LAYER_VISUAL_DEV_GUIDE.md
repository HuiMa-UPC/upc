# 首层视觉检测开发指南（Visual 团队）

本文面向视觉算法开发同学，说明当前首层检测链路、需要对接的代码入口，以及联调注意事项。

## 1. 功能目标

- 在**首层**下位机上报 `PRINT_READY` 后，由 Glavo 触发 Visual 做一次检测。
- 打印开始且开关开启时，Glavo 先通知 Visual 启动 Camera 取流并预热定位缓存。
- Visual 从 Camera 的 dma fd + SHM ring 拿到最新 NV12 原图，执行检测逻辑。
- Visual 回传检测结果：
  - `result=1`：首层通过，Glavo 继续出光打印。
  - `result=0`：首层不通过，Glavo 发送 `LAYER_DONE(layer_height_um=0)` 继续铺粉，不降基板。
- 仅首层生效；首层通过后，后续层走原流程，不再触发视觉检测。

## 2. 运行时开关（UI 调试页）

- 位置：Debug 页面 `Other` 页签中的 `1st Layer Visual` 开关。
- 默认值：关闭。
- 消息契约：
  - `UI_Debug_FirstLayer_Visual_Check_Set_Ntf`
  - `UI_Debug_FirstLayer_Visual_Check_Get_Ntf`
  - `GLAVO_Debug_FirstLayer_Visual_Check_Ntf`

关闭时：打印流程与历史逻辑一致，完全跳过首层视觉检测。

## 3. IPC 契约（Glavo ↔ Visual）

定义位置：`common/upc_contract.hpp`

- 会话开始：`GLAVO_Visual_FirstLayer_Session_Start_Ntf`（`0x1425`，Empty）
- 会话停止：`GLAVO_Visual_FirstLayer_Session_Stop_Ntf`（`0x1426`，Empty）
- 裸板回执：`APP_Visual_FirstLayer_Reference_Ready_Rsp`（`0x1427`）
  - 载荷：`VisualFirstLayerReferenceReadyPayload`
  - Visual 成功锁定本次打印 B0 前，Glavo 不得开始运动；失败/超时必须终止起印
- 请求：`GLAVO_Visual_FirstLayer_Check_Req`（`0x1423`）
  - 载荷：`VisualFirstLayerCheckReqPayload`
  - 字段：`layer_height_um`
- 响应：`APP_Visual_FirstLayer_Check_Rsp`（`0x1424`）
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
- 算法入口：`VisualService::process_first_layer_image(...)`
- 分割定位、透视矫正、光照预处理和融合分类：
  - `FirstLayerDetector::observe_nv12(...)`
  - `FirstLayerDetector::predict_nv12(...)`

Visual 空闲时不取流、不分割、不分类；会话停止后释放 Camera dma-buf/SHM，保留已加载的 RKNN 上下文。

### 4.2 Glavo 侧

文件：`app/glavo/lib/glavo_service.cpp`、`app/glavo/lib/mcu_link.cpp`

- Glavo 发送请求并等待响应：`GlavoService::request_first_layer_visual_check()`
- Glavo 接收 Visual 回包：`GlavoService::handle_visual_first_layer_check_rsp()`
- 首层门控逻辑（收到首层 `PRINT_READY` 后）：
  - `McuLink::link_process_serial_handshake()`

## 5. 算法接入建议

模型、预处理和几何定位只允许放在 Visual，Glavo 只负责打印状态和 IPC 控制：

1. 在 `FirstLayerDetector` 中维护真实检测逻辑，不要把 RKNN/OpenCV 模型代码放进 Glavo。
2. 入参使用已拷贝的紧凑 NV12 数据（`width * height * 3 / 2`）。
3. 根据检测结果返回：
   - `true` → `result=1`
   - `false` → `result=0`
4. 如需更多调试信息，先通过日志输出，不要直接改动跨进程载荷字段。

## 6. 上机推理报告

每次检测会在 `rongsu_visual` 日志中写一条 `first layer inference report_v=1` 结构化记录，
包含合格概率、当前阈值、置信度、OOD、定位缓存、拒绝原因以及分割/预处理/分类耗时。
不会为每次检测单独创建日志或报告文件。最终 `result=1` 时，每个首层会话最多保存一张
诊断合成图到 `/home/media/first_layer_visual/qualified/`：左侧是完整相机帧，右侧是透视
矫正后的模型基板区域。目录仅保留按文件名排序的最近 200 张。
测试结束后可在电脑端执行：

```powershell
python tools/collect_first_layer_board_report.py `
  --serial 172.17.101.16:5555
```

脚本会拉取三份轮转日志、合格诊断图、板端程序/模型哈希和几何缓存，一次生成一个
汇总 CSV、一个 Markdown 报告以及 `0.50～0.85` 阈值回放表。该报告没有人工真值时
只能评价模型行为与耗时，不能代替准确率、召回率和误放率统计。

## 7. 联调检查清单

- 开关关闭：
  - 打印应与旧流程一致，不触发视觉请求。
- 开关开启 + Visual 返回 1：
  - 首层等待后继续正常打印。
  - Visual 停止 Camera 取流。
- 开关开启 + Visual 返回 0：
  - 不出光，且反复触发 `LAYER_DONE(0)` 继续铺粉，直到返回 1。
  - Visual 保持取流并继续更新定位缓存。
- 首层通过后：
  - 后续层不再触发视觉检测请求。

## 8. 注意事项

- Visual 图像数据来自 Camera 并行流，依赖会话开始通知后发送的 `VISUAL_Camera_Stream_Start_Req`。
- 首层等待期间，Glavo 最多等待 Visual 3 秒；超时按 `result=0` 保守处理，避免打印线程永久阻塞。
- 若后续要扩展返回内容（置信度、缺陷类型等），请按 `common/COMMON.md` 的契约四步流程新增消息字段。

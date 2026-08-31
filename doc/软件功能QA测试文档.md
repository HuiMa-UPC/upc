# Snowpod 软件功能 QA 测试文档

> 文档版本：v1.4  
> 适用对象：设备端软件 `rongsu_ui`、`rongsu_glavo`、`rongsu_camera`、`rongsu_media`、`rongsu_xfr`、`rongsu_visual`、`rongsu_mqtt`  
> 测试类型：功能测试、基础异常测试、跨进程联调  
> 更新日期：2026-08-22

## 本次更新说明（v1.4）

本次版本主要更新以下内容，请在回归时优先关注：

1. **打印录像/延时摄影 finalize 时序调整**：改为可分阶段收尾（先 Timelapse，后 H264），需验证 pending 状态与可播放时机是否符合预期。
2. **0 层终止误删历史防护**：新增对非法 `frames_dir`（误传 `.ovf` 文件名）的保护，避免 `commit miss` 触发历史录像回收删除。
3. **历史录像回收风险回归**：补充“开始打印后立即终止”场景，验证不会删除既有历史记录。
4. **视频播放器稳定性修复**：针对长视频播放抖动/黑屏/崩溃风险，补充长时播放、频繁拖动和进出页面压力测试。
5. **媒体目录策略变更**：启动仅初始化 `/home/media/*` 目录，不再做 `/home/video`、`/home/photo`、`manifest.json` 旧布局迁移。
6. **打印启动链路优化（OVF）**：Prepare 阶段复用解析器会话，减少 Start 阶段重复 `open_file` 开销，重点回归“大 OVF 点击开始后首条串口起印指令时延”。
7. **环境传感器协议升级**：`0xA5/0x9B/TLV 0x02` 支持新旧长度兼容（旧版 13 字节、新版 21 字节），新增 `PM1.0 / PM10 / 滤芯气压 / 舱内2气压` 解析。
8. **传感器日志去重与口径统一**：移除重复的 `RX SENSOR OK` 详细日志，统一以 `sensor:` Info 日志输出；气压按协议口径 `0.1kPa` 解析并展示。
9. **相机预览（PHX）RGA streaming 优化**：预览分块默认 512 行，新增 `import/mmap` 缓存与源直裁优先（失败回落 memcpy 分块），验证 `rga_path=1` 下转换耗时与稳定性。
10. **跨工具链条件编译边界修复**：`improcess/imsetColorSpace` 等仅在 `UPC_UI_RGA_STREAMING=1` 编译，需回归 `k1mini` 与 `phx` 两套工具链构建结果。
11. **预览日志口径增强**：新增/保留 `decode 5s` 统计（`ok/fail/no_frame/cvt_avg_us/rga`），用于对比优化前后性能。
12. **预览与 UI 动画并发场景**：监控页存在实时大图刷新与滑动解锁/弹窗动画竞争，需补充流畅度与掉帧观测。
13. **MQTT 激活失败重试策略优化**：激活失败重试间隔由固定 5s 调整为分级退避（5s/10s/30s/60s）；激活成功后失败计数清零。
14. **日志分析工具新增**：新增 `tools/analyze_topic_logs.py`，支持批量解析 `topic*.log`，输出 `summary.json`、多类 `metrics_*.csv`、`report.html`、静态图和可交互环境曲线。

## 1. 测试范围与结论口径

本测试文档依据当前仓库代码编写，覆盖已具备用户入口或对外接口的功能。测试通过不仅要求界面有预期反馈，还要求后端实际完成对应动作或返回正确结果。

以下能力在当前版本中属于占位、仅有 UI 外壳，或后端尚未实现，**不应按“已交付功能”验收**：

- 左侧导航的 `Control`、`Message` 页面为 skeleton 占位。
- 设置页 `Display & Sound`、`Advanced Print`、`Privacy & Security` 显示 `coming soon`。
- 未连接 Wi-Fi 条目的右箭头页面显示 `coming soon`。
- 调试页的 Axis Jog 后端标记为待实现；Galvo Move 为预留 TCP 能力。
- 传输盒的“装盒到位”使用定时器模拟；转运失败/中止后会停留在转圈页面，无自动超时或重试。
- Wi-Fi 传输 PIN 弹窗的取消仅关闭 UI，不会取消 Xfer 后端配对会话；PIN 过期也不会自动关闭弹窗。
- 视觉识别依赖 OpenCV；缺少 OpenCV 时仅可验证 `valid=0` 的降级结果。

## 2. 测试环境与通用准备

| 项目 | 要求 |
| --- | --- |
| 设备 | 已连接显示屏、MCU、相机；涉及运动/激光的用例必须在安全工装和授权环境执行 |
| 工具链 | 至少准备 `k1mini-buildroot-toolchain` 与 `phx-buildroot-toolchain` 两套构建环境，用于跨工具链编译回归 |
| 系统服务 | 使用 `scripts/rongsu.sh start` 启动；启动顺序为 Glavo、Camera、Media、Xfer、Visual、MQTT、UI |
| 网络 | Wi-Fi 用例准备一个正确密码热点和一个错误密码场景；Xfer/MQTT 用例要求测试机与设备处于可达网络 |
| 文件 | 准备有效 `.gcode`、有效 `.ovf`、损坏/不支持格式文件；准备 USB 存储介质 |
| 存储 | 确保 `/home/Gcode`、`/home/media/photo`、`/home/media/video` 可写；容量测试需能构造 90% 与 100% 水位 |
| 相机 | 相机可正常出流；录像、拍照和视觉用例需有可观察画面 |
| 日志分析（V4.0.4） | 准备可在 Windows 或 Linux 运行 Python3 的主机；基础解析仅需标准库，静态图需 `matplotlib`，交互图需 `plotly` |
| 可观测性 | 统一专题日志目录 `/var/log/topic/*.log`（按串口、TCP、IPC、MQTT、相机、UI、存储、系统监控等专题拆分） |

通用判定原则：

1. 对危险动作（开门、运动、激光、风机、OTA）先确认设备处于安全状态；执行人应具有设备操作授权。
2. 跨进程操作需同时检查 UI 表现、目标进程日志和最终数据文件，不能仅以按钮响应判通过。
3. 出现未定义异常时，保留截图、操作时间、复现步骤及相关进程日志；不得擅自重启后直接判定不可复现。

## 3. 测试数据

| 编号 | 测试数据 | 用途 |
| --- | --- | --- |
| D01 | 可正常打印的本地 `.gcode` | 本地文件、预处理、打印主流程 |
| D02 | 可正常解析的 `.ovf` | Xfer 上传；是否可直接打印取决于当前设备构建是否启用 OVF 解析器 |
| D03 | 损坏/截断的 G-code 或 OVF | 解析失败与异常提示 |
| D04 | 不支持扩展名，例如 `.txt` | 上传格式拦截 |
| D05 | 正确与错误 Wi-Fi 密码 | Wi-Fi 连接正反用例 |
| D06 | 可访问 MQTT Broker、激活服务和 RTSP 接收端 | MQTT/云端直播 |
| D07 | MCU OTA 文件 `/home/image/galvo`、串口 OTA 文件 `/home/image/mcu` | OTA 用例 |

## 4. 功能测试用例

### 4.1 启动、首页与环境状态

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-BOOT-01 | 全量启动 | 执行启动脚本，等待 UI 出现首页。 | UI 可进入首页；必需的 Glavo 连接成功；各进程无持续崩溃或反复重启。 |
| F-BOOT-02 | 后端未就绪 | 停止 Camera 或 Glavo 后启动 UI。 | UI 不闪退；相机相关功能不可用或提示异常；Glavo 不可用时打印动作不能假成功。 |
| F-HOME-01 | 常驻状态条 | 在 Home、History、Settings、Control、Message 五个导航页间切换。 | 均显示 Wi-Fi、设备名、运行状态；隐藏页（文件、调试、延时播放）不显示该状态条。 |
| F-HOME-02 | 环境值刷新 | 让 MCU 上报有效环境值，再上报无效值（负数）。 | 首页和状态相关区域更新；无效环境指标显示 `--`，不显示误导性数值。 |
| F-HOME-02A | 0x9B 新旧环境帧兼容 | 注入两种 `Port=0xA5 Cmd=0x9B TLV=0x02`：场景 A `Tlv-Len=13`（旧版）；场景 B `Tlv-Len=21`（新版，含 PM1.0/PM10/滤芯气压/舱内2气压）。 | 两种帧均可解析且不报 `RX SENSOR FAIL ... TLV`；旧版仍更新 O2/舱内1气压/温湿度/PM2.5/粉位/基板温度；新版在此基础上新增解析 PM1.0、PM10、滤芯气压、舱内2气压。 |
| F-HOME-03 | 状态锁定 | 开始打印、暂停、结束或清粉后观察状态条，再注入普通环境推送。 | `Printing`、`Paused`、`Print Complete`、`Print Terminated`、`Air Purifying`、`Purify Complete` 不被环境推送覆盖。 |
| F-HOME-04 | 导航与页面缓存 | 在打印中切到设置页，再回首页。 | 可切换页面；首页打印浮层及进度保持，不被重建或丢失。 |

### 4.2 文件选择与打印准备

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-FILE-01 | Local 清单加载 | 通过成功的 Xfer 上传、USB 预处理或受控测试夹具，使 `/home/Gcode/config.json` 存在 D01 的有效条目且对应文件存在；打开 My Files 的 Local Tab。另在目录中放入一个未写入清单的文件后刷新列表。 | Local Tab 仅展示 `config.json` 中的条目及其文件名、时长、耗材和大小；未写入清单的目录文件不得显示。若清单缺失、无效或为空，列表为空且 UI 不崩溃。 |
| F-FILE-02 | USB 文件列表 | 插入含 D01/D02 的 USB，切至 USB Tab 并刷新。 | 显示 USB 可支持文件；拔出 USB 或空盘时列表正确刷新为空/提示。 |
| F-FILE-03 | 本地删除 | 选择 Local 文件，执行删除并在确认框确认。 | 文件从磁盘和 `/home/Gcode/config.json` 移除，列表同步刷新。取消确认时文件不得删除。 |
| F-FILE-04 | 启印入口与下发 | 选中 D01，观察 `Confirm Print` 按钮状态并完成二次确认开始打印。每次换文件、切换 Local/USB、取消选中文件及删除本地文件后，重新观察按钮状态。 | 选中文件后 `Confirm Print` 可点击，点击后可进入二次确认。发起 `UI_Print_Start_Req` 时仅携带文件索引与来源，不再包含厚度字段。 |
| F-FILE-05 | 预处理成功（Local/USB） | 场景 A：在 Local 选择 D01，完成二次确认。场景 B：USB 插入一个当前构建支持打印的文件，执行同样操作。观察 `Preparing Print File` 弹窗、UI 结果和 `/home/Gcode/config.json`。 | Local 文件存在且可扫描时，后端复用或扫描行数并更新清单；USB 文件先复制到 `/home/Gcode`，再扫描并写入清单。两种场景均收到 `APP_Print_Prepare_Rsp(ok=1)`，准备弹窗关闭并自动进入打印流程。D02 仅在设备已启用 OVF 解析器时纳入该用例。 |
| F-FILE-05A | OVF 大文件起印时序 | 选择较大的 D02（OVF），完成二次确认并开始打印；记录 `print_prepare ok`、`print_start: path=...`、`serial TX cmd: PRINT_REQUEST`、`serial TX cmd: PRINT_START` 时间戳。 | Start 阶段不应再出现明显的二次 `open_file` 长阻塞；`print_start` 与首条串口起印握手日志间隔应显著缩短。若总耗时仍较长，应主要体现在 Prepare 阶段而非 Start 阶段。 |
| F-FILE-06 | 预处理失败（当前无 UI 取消） | 选择 D03，或选择目录清单中存在但实际已删除/不可读的文件；完成二次确认。观察准备弹窗、提示和后端日志。 | 当前准备弹窗没有取消按钮，测试不应要求“用户取消”。后端返回 `APP_Print_Prepare_Rsp(ok=0)` 后，弹窗关闭、UI 提示 `Prepare failed` 并回到 Idle；不得进入打印状态或向 MCU 开始下发运动数据。若使用 USB 文件，是否保留已复制文件按当前实现记录，不作为本用例失败判据。 |
| F-FILE-07 | 打印占用门禁 | 打印结束但尚未完成取件时尝试进入 My Files 并新建任务。 | 文件页入口被阻止，不能启动第二个打印任务。 |

### 4.3 打印控制与异常流程

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-PRINT-01 | 正常开始 | 以 D01 完成预处理与二次确认，开始打印。 | UI 进入打印态；Glavo 开始解析和下发；进度、剩余时间与相机直播可用。 |
| F-PRINT-02 | 进度展示 | 让后端推送 0%、中间值、100% 进度。 | 百分比和进度条单调更新；打印中显示 `TIME LEFT`，无耗材数据时显示 `--`。 |
| F-PRINT-03 | 暂停与恢复 | 打印中依次执行暂停、恢复。 | 状态切为 `Paused` 后恢复为 `Printing`；后端 TCP 运动下发相应停止/恢复；重复点击不会形成状态错乱。 |
| F-PRINT-04 | 用户终止 | 打印中点击终止，在确认框确认；另测取消确认。 | 确认后停止解析/下发并进入 `PRINT TERMINATED` 结果态；取消时打印继续。 |
| F-PRINT-05 | 自然完成 | 执行一个可完成作业。 | 进度到 100% 后进入结果态，显示 `PRINT COMPLETE`；结果态时间为 `TIME COST`，数据存在时显示耗材量。 |
| F-PRINT-06 | 缺粉告警 | 打印中模拟粉量位 bit0/bit1 同时为 0。 | 后端暂停并只推一次缺粉告警；UI 弹补粉框。点击补粉恢复后仍缺粉时应再次暂停并重新弹框。 |
| F-PRINT-06A | 新版粉位位图兼容 | 使用新版 `TLV_SENSOR_REPORT_1ST`（含完整位图）分别构造：A `powder_bits=0x00`，B `powder_bits=0x01/0x02/0x03`。 | 缺粉判定保持旧规则：仅 `bit0/bit1 == 00` 触发缺粉；其余值不触发缺粉。日志中的 `powder_bits` 与 `powder_ok` 与注入值一致。 |
| F-PRINT-07 | 缺粉取消 | 在缺粉弹窗点击 Cancel，再通过底部恢复按钮尝试恢复。 | Cancel 只关闭提示并保持暂停；恢复后仍由后端粉量判定是否允许继续。 |
| F-PRINT-08 | 结果页拍照 | 打印中、完成、终止三种状态分别点击拍照。 | Media 保存 PNG 至 `/home/media/photo`；有条件时生成同名 `.rthm`；UI 显示成功/失败反馈。 |
| F-PRINT-09 | 取件与关门 | 结果页滑动 `SWIPE TO COLLECT`，完成开门、取件、关门确认。 | 正确经历清粉/开门/关门流程，最终回到 Idle；未确认关门前不得提前收掉结果上下文。 |
| F-PRINT-10 | 清粉流程 | 在结果页点击吹粉，观察运行和完成状态。 | 状态依次为 `AIR PURIFYING...`、完成提醒；不会把主打印徽标错误改为其他状态。 |
| F-PRINT-11 | 转运盒流程 | 在调试页启用 Transfer Box，完成一次取件。 | 依次出现装盒、转运、推闸门及成功提示；收到成功结果才可进入推闸门。失败/中止会停在转圈页，记录为当前已知行为。 |
| F-PRINT-12 | 0 层终止不误删历史 | 选择任意作业后点击开始，确保未发生层间拍照即终止；重复 3 次。每次终止后查看 Print History 总条目数与既有旧记录目录。 | 不应出现历史录像被批量删除；`print_video commit miss` 场景不得触发 `STORRCLM_SESSION_DEL` 连续删除。 |

### 4.4 相机预览、媒体与历史

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-CAM-01 | 开关预览 | 首页进入打印模式开预览，切换离开或停止预览。 | 画面持续刷新，无明显黑屏；停止后 Camera 移除 UI 会话，无其他消费者时可停止采集。 |
| F-CAM-02 | 相机恢复 | 出流后断开/阻塞相机超过看门狗阈值，再恢复。 | Camera 自动重建采集、SHM 和 DMA fd；UI 恢复画面，无需人工重复开关流。 |
| F-CAM-03 | 层间录像完成 | 正常完成含多层作业。 | 生成 PNG 序列、缩略图和历史清单；Media 发 manifest 通知后 UI 进入结果页。 |
| F-CAM-04 | 层间录像终止/零帧 | 分别在有帧和刚开始未产生帧时终止。 | 有帧作业保留终止历史；零帧也必须退出打印界面，不能永久停在打印中。 |
| F-CAM-05 | 分阶段 finalize 联动 | 开启 Print H264 Record，执行“有帧后终止”并进入取件流程；观察 timelapse 与 print_h264 状态变化。 | Timelapse 可先完成并可播放；H264 完成后再清 `print_video_pending`，最终 print_h264.mp4 与 print_thumb 正常落盘。 |
| F-CAM-06 | PHX 预览 RGA streaming 稳定性 | 使用 `phx` 构建并进入监控预览，持续运行 ≥ 2 分钟；检查 `topic_ui_preview.log`。 | `decode ready` 显示 `rga_path=1`；`decode 5s` 持续 `ok≈150/5s`、`fail=0`、`no_frame=0`、`rga=1`；`cvt_avg_us` 稳定（建议基线约 6~8ms）。 |
| F-CAM-07 | 预览并发动画流畅度 | 监控预览运行中反复触发滑动解锁、弹窗切换、页面进出动画；同时观察 `decode 5s` 与主观流畅度。 | 预览不中断且 `fail=0`；动画无持续性卡顿（允许偶发轻微抖动）；若出现明显掉帧需记录时段并关联 `cvt_max_us/loop_max_us` 峰值。 |
| F-CAM-08 | 跨工具链编译边界 | 分别使用 `k1mini` 与 `phx` 工具链执行全量构建。 | 两套构建均成功；`k1mini` 不应出现 `improcess/imsetColorSpace/IM_SYNC` 相关编译或链接错误；`phx` 保持预览 streaming 路径可用。 |
| F-HIST-01 | 历史分组与状态 | 打开 Print History，准备完成和中断的多条记录。 | 按开始时间倒序、按自然日分组；状态、时长、耗材和缩略图与清单一致。 |
| F-HIST-02 | 延时摄影播放 | 在 Timelapse 点击缩略图，执行播放、暂停、拖动、关闭。 | 打开全屏播放器；拖动时显示预览，松手后定位主画面；关闭后回到 History。 |
| F-HIST-02A | 长视频 MP4 稳定性 | 选择长时 MP4（建议 >= 10 分钟），连续执行播放/暂停、快速拖动、来回切页（History <-> Timelapse）各 10 次。 | 不应出现黑屏、明显抖动积累或进程崩溃；返回页面后播放器可继续打开与操作。 |
| F-HIST-03 | Album 浏览和删除 | 通过拍照生成照片，打开 Album；单项左滑删除及管理态批量删除。 | 删除 PNG 与配对 `.rthm`；仅删除当前 Tab 对应资产，不应误删另一类录像资产。 |
| F-HIST-04 | 历史批量删除 | 在 Print History 和 Timelapse 分别多选、删除并确认/取消。 | 确认后仅删除所选类型资产及必要清单项；取消不删除；`video_pending` / `print_video_pending` 条目不可删除。 |
| F-HIST-05 | 空态 | 删除当前 Tab 最后一条记录。 | 显示正确空态；Print History 空态提供 Start Print，Timelapse/Album 空态不提供该按钮。 |
| F-HIST-06 | 存储容量提示 | 构造 <90%、≥90%、100% 三个使用率。 | 容量条颜色依次为正常、橙色、红色；≥90% 出现告警横幅；100% 不会导致 UI 异常。 |
| F-HIST-07 | 旧布局不迁移验证 | 在升级设备上保留旧路径 `/home/video`、`/home/photo`、`manifest.json`（若有），上电后执行一次打印与历史浏览。 | 系统仅使用 `/home/media/*`；旧路径不会自动迁移。若旧资产仍需保留，按升级方案手工迁移并记录。 |

### 4.5 设置、网络与 OTA

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-SET-01 | Wi-Fi 开关与扫描 | 设置 → Network & Connectivity，切换 On/Off，观察扫描列表。 | 开启后请求扫描并显示真实网络；关闭后隐藏列表和翻页按钮，扫描缓存清空。 |
| F-SET-02 | 新网络连接 | 选择未保存 SSID，分别输入 D05 的正确/错误密码。 | 正确密码显示 Connected 并刷新当前 SSID/IP；错误密码关闭连接中弹窗并提示 Connect failed。 |
| F-SET-03 | 已保存网络 | 连接已保存 SSID；进入 Saved Networks 后忘记某条；在当前网络详情执行 Disconnect 和 Forget。 | 已保存网络免密连接；忘记后列表刷新；断开后不再显示当前网络详情。 |
| F-SET-04 | Wi-Fi 页面刷新 | 停留在网络页，改变附近热点或后端连接状态。 | 定期扫描后列表更新；网络条目未变化时不应频繁跳回顶部。 |
| F-SET-05 | About 信息 | 打开 About，等待 MCU 版本返回；再模拟超时。 | 展示软件版本、设备型号、MCU 版本；首次超时后重试一次，仍失败显示 Unknown；已有有效缓存不被失败覆盖。 |
| F-SET-06 | OTA 成功、失败、取消 | 分别触发 Glavo OTA 与 MCU Serial OTA，模拟进度、成功、失败和点击关闭取消。 | 弹窗显示状态和 0–100% 进度；成功/失败文案准确；关闭按钮发送对应取消请求。仅在安全授权环境执行真实升级。 |
| F-SET-07 | 调试入口密码 | 点击 Debug，输入错误密码、正确密码 `888`，退出后再次进入。 | 错误密码提示且清空输入；正确密码进入调试页；同一次 UI 运行内再次进入无需重复输入，重启 UI 后需重新验证。 |

### 4.6 Wi-Fi 文件传输（Xfer）

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-XFR-01 | 服务发现与信息 | 在局域网浏览 `_snowpod._tcp`，访问 `GET /api/v1/info`。 | 能发现设备；服务返回 SN、model、api_version、need_pair、state。 |
| F-XFR-02 | PIN 配对 | 调用 `/pair/start`，在 UI/设备获取 PIN，依次提交正确、错误、过期 PIN。 | 正确 PIN 获得 token；错误 PIN 返回拒绝，超过限制后会话锁定；过期返回过期结果。 |
| F-XFR-03 | 鉴权与撤销 | 使用有效 token 调受保护接口，再撤销 token 后重复请求。 | 有效 token 可访问；撤销后请求被拒绝，客户端需重新配对。 |
| F-XFR-04 | OVF 分块上传 | 对 D02 调用 init、按 Content-Range 分块 PUT、complete。 | 返回连续 offset；完成后 SHA-256 校验通过，文件落入 `/home/Gcode` 且清单更新。 |
| F-XFR-05 | 断点续传与校验失败 | 上传中断后重新 init/status 继续；使用错误 SHA-256 complete。 | 可从服务端 offset 继续；校验失败返回错误且不产生可打印正式文件。 |
| F-XFR-06 | 格式与进度 UI | 上传 D04；上传 D02 时观察 UI 传输弹窗。 | 非 `.ovf` 被拒绝；Uploading/Slicing 时显示进度，Finished/Error/Idle 自动关闭，完成后文件列表刷新并提示。 |

### 4.7 MQTT 云端能力

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-MQTT-01 | 设备激活与重试 | 删除或置为无效 MQTT 凭据，启动 MQTT；先使激活服务不可达，再恢复。 | 无凭据时发起激活；失败按周期重试；成功后保存凭据并连接/订阅命令主题。 |
| F-MQTT-01A | 激活失败退避间隔 | 删除 MQTT 凭据并持续阻断激活服务，观察 `topic_mqtt_telemetry.log` 中 `device activation failed ... retry #N in Xms`；至少连续观察 4 次失败。 | 日志重试间隔符合：第 1 次失败后 5s、第 2 次后 10s、第 3 次后 30s、第 4 次及以后 60s。 |
| F-MQTT-01B | 激活成功后失败计数清零 | 先制造连续激活失败（计数 >=2），随后恢复激活服务直至成功，再次阻断并观察下一次失败日志。 | 成功后应重置失败计数；再次失败应从 `retry #1 in 5000ms` 重新开始，而非延续之前的大间隔。 |
| F-MQTT-02 | 环境遥测 | 分别在 Idle 和 Printing 注入环境快照，订阅 `telemetry/chamber`。 | 发布 JSON 含 device_id、ts、seq、pm25、o2_ppm、ntc_c；打印中发布频率高于待机；无快照时不发伪数据。 |
| F-MQTT-02A | PM10 透传一致性 | 注入新版环境帧（含 PM10），同时观察 UI 首页与 MQTT 遥测。 | UI 的 `pm10_indoor` 显示与上报 PM10 一致；无 PM10 的旧版帧保持兼容，不导致 UI/MQTT 异常。 |
| F-MQTT-03 | 远程灯光控制 | 向命令主题发送有效 `set_light` 开/关与亮度命令。 | Glavo 收到对应设备控制；`cmd/ack` 返回 accepted 或 error，req_id 与 cmd 一致。 |
| F-MQTT-04 | RTSP 推流控制 | 向命令主题发送有效 `start_rtsp_push`、`stop_rtsp_push`。 | MQTT 将请求转给 Media；Media 结果最终映射为 `cmd/ack`；开始后 RTSP 接收端可见画面，停止后流结束。 |
| F-MQTT-05 | 非法命令 | 发送缺 req_id、未知 cmd、错误 type、字段不合法或重复 req_id。 | 有 req_id 时返回 rejected；无 req_id 不发无法关联的 ack；重复待处理 req_id 被拒绝，服务不崩溃。 |
| F-MQTT-06 | 断线恢复 | MQTT 连接后停止 Broker，再恢复 Broker。 | 服务识别发布/订阅失败并重连；重连后重新订阅命令主题，后续遥测和命令可用。 |

### 4.8 调试与维护（受控环境）

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-DBG-01 | 状态回读 | 进入调试页，查看传感器、轴、设备、振镜、激光状态。 | UI 数值随 Glavo 推送刷新；断开后端时不应显示伪成功或导致 UI 崩溃。 |
| F-DBG-01A | 传感器日志去重与字段完整性 | 连续接收环境上报，检查 `rongsu_glavo` 日志。 | 不再出现重复详细数值的 `RX SENSOR OK ...`；保留单条 `sensor:` Info 日志，并包含 O2、P1、T、RH、PM1.0、PM2.5、PM10、滤芯气压、舱内2气压、基板温度、powder_bits、powder_ok、door_open。气压单位按 kPa 口径展示。 |
| F-DBG-02 | 手动设备控制 | 在安全工装下测试门、电机、风扇、激光、红光、粉桶/筛粉/内门等单项开关及停止。 | 实际设备动作与 UI 指令一致；停止动作可终止连续动作；异常状态有日志可追溯。 |
| F-DBG-03 | 参数与报警维护 | 测试风机占空比范围 35–100、激光功率 0–100、功率复位、激光报警复位、振镜标定/参数保存。 | 合法边界值被接受；越界值被拦截或按协议处理；复位后恢复外控；标定/保存结果可从状态或日志确认。 |
| F-DBG-04 | 环境模拟 | 开启 Env Sim，验证环境值变化；切换模拟缺粉，等待 5 秒；关闭 Env Sim。 | 开启后 UI/MQTT 使用模拟环境数据；缺粉延迟生效；关闭后恢复真实串口环境数据。 |
| F-DBG-05 | 录像调试开关 | 打开/关闭 Print H264 Record、Clean Record，执行一次对应流程。 | 状态回读与设置一致；开启时产生对应媒体资产，关闭时不产生不应有的录像条目。 |

### 4.9 日志分析工具（V4.0.4）

| ID | 测试项 | 步骤 | 预期结果 |
| --- | --- | --- | --- |
| F-LOG-01 | 目录输入与批量解析 | 在测试机执行：`python tools/analyze_topic_logs.py --input <topic日志目录> --output <输出目录>`，其中输入目录包含多个 `topic*.log`（可嵌套子目录）。 | 脚本成功结束并输出 `[OK] parsed files/lines`；输出目录生成 `summary.json`、`key_events.csv`、`metrics_env.csv`、`metrics_camera_capture.csv`、`metrics_ui_preview.csv`、`metrics_system_resource.csv`、`metrics_mqtt.csv`、`report.html`。 |
| F-LOG-02 | 环境全字段与交互曲线 | 日志包含 O2、P1、T、RH、PM1.0、PM2.5、PM10、Pfilter、P2、base、powder_bits、door 等字段，执行脚本并打开 `env_interactive.html`。 | 交互页面可缩放/拖动/滚轮放大；多子图展示全量环境字段；`metrics_env.csv` 中字段完整且时间戳单调。 |
| F-LOG-03 | 可选依赖退化行为 | 分别在未安装 `matplotlib`、未安装 `plotly`、两者都安装三种环境执行脚本。 | 未安装依赖时脚本不中断，并给出安装提示；安装后分别生成静态图（`plots/*.png`）和交互图（`env_interactive.html`）。 |
| F-LOG-04 | 关键异常归并正确性 | 构造包含 `PROCESS_BOOT prev_clean=0`、`RGA fallback`、`capture release_timeout>0`、`MQTT connect failed` 的样例日志并执行脚本。 | `summary.json` 的 `alerts` 和 `report.html` 的 Top Alerts 能归并对应告警域（BOOT/UI_PREVIEW/CAMERA_CAPTURE/MQTT），且时间和原日志一致。 |

## 5. 回归优先级

| 优先级 | 必测范围 | 触发条件 |
| --- | --- | --- |
| P0 | F-BOOT、F-FILE、F-PRINT、F-CAM、F-HIST、F-SET-01~05 | 任意 UI、IPC、Glavo、Camera、Media、文件清单改动；涉及 `camera_preview` 时需加测 F-CAM-06/07/08 |
| P1 | F-XFR、F-MQTT、F-SET-06~07、F-LOG | 网络、传输、云端、OTA、认证与日志分析相关改动 |
| P2 | F-DBG、视觉识别、视觉效果/布局 | 调试、协议、OpenCV、UI 样式改动 |

## 6. 缺陷记录要求

每条缺陷至少包含：

- 用例 ID、软件版本、设备型号和测试环境。
- 完整操作步骤、实际结果和期望结果。
- UI 截图或录像，以及发生时间（精确到分钟）。
- 相关进程日志；跨进程问题至少提供 UI 与目标后端日志。
- 是否可稳定复现、复现概率，以及是否涉及安全风险或数据丢失。

## 7. 代码依据

- 架构与进程边界：`doc/ARCHITECTURE.md`
- UI、打印流程、历史、预览、设置功能：`app/ui/UI.md`
- Glavo 打印和 MCU 编排：`app/glavo/GLAVO.md`
- 相机、拍照、录像、媒体落盘：`app/camera/CAMERA.md`
- Xfer 服务接口与上传约束：`app/xfer/XFER.md`
- MQTT 遥测和命令：`app/mqtt/lib/mqtt_service.cpp`
- 日志分析工具：`tools/analyze_topic_logs.py`
- IPC 消息、OTA、调试能力：`common/COMMON.md`

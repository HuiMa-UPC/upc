# 基板完整流程临时分支验证记录

软件测试快照日期：2026-09-24。分支：`test/plate-fullflow-20260924`，基线：`gsx/860e4a1`，当时构建的测试包版本：`V4.2.0.2-plate-test1`。

该测试分支继承 `gsx` 已有的 SOC/UI 完整起印检查、无板提示及复检代码，新增内门／基板共用的内置参数，并修正会话编号和回复等待衔接。下表是 2026-09-24 的软件测试快照；测试包之后已部署并进行部分现场场景测试，但本报告未汇总完整设备日志，软件测试不能证明机构全流程已现场通过。
接入约定见 [CABIN_IPC_INTEGRATION.md](CABIN_IPC_INTEGRATION.md)，复现命令见
[app/visual/tests/README.md](../app/visual/tests/README.md)。

## 已验证

| 项目 | 结果 | 范围 |
|---|---|---|
| 协议测试 | PASS | 六条消息号、可靠登记、4/16/16 字节 ABI、字段布局 |
| 单帧算法／连续证据测试 | PASS | 严格标定、坏输入、重载失效、黑帧、合成标记、旧／重复帧、序号回绕 |
| 隔离 IPC 集成测试 | PASS | 实际 Broker/VisualService、模拟 Camera NV12、SHM、SCM_RIGHTS；请求／回复／停止／再检／取消／超时／缓存／冲突／15 秒闲置释放 |
| Visual CTest 汇总 | 5/5 PASS | Ubuntu、GCC 13.3、OpenCV 4.6；25.85 秒；新增内置参数／模板测试，内置 IPC 子用例实际执行 |
| SOC 通信回归 | 1/1 PASS | 生产实际使用的共享编号与回复等待 helper；即时回复、重复／迟到、超时、发送失败、终止竞态、损坏确认包；0.01 秒 |
| aarch64 交叉编译 | PASS | `/tmp/upc-plate-fullflow-cross`，phx Buildroot GCC 13.3、OpenCV 4.9；八进程及附属目标全部构建成功 |
| 测试应用包 | PASS | 平铺 10 文件，9 个载荷校验与产物一致；manifest、UI、MQTT 版本一致为 `V4.2.0.2-plate-test1` |
| 契约边界 | PASS | 本轮相对 860e4a1 不修改 common 契约数值、载荷或传输实现 |
| 改动边界 | PASS | 当时未改其他工作树；内置参数和 SOC 通信修正未改变 SOC 门／Z 策略 |

隔离测试使用独立临时 socket 和 SHM，没有接入正在运行的设备 Broker。
程序构建成功只证明编译通过，不证明当前机器固件与产物可直接混用。

Visual 集成测试中父进程模拟 SOC；SOC 新测试针对实际生产 helper，并没有运行真实 Glavo 的机构流程。两组通过不能冒充完整 UI→SOC→MCU→机构→相机的实机端到端验收。

## 已有现场录像抽帧复核

本次把原项目中四张 PNG 转为紧凑 NV12，调用 C++ 单帧函数；使用 `cabin_frame_check --bundled`，不传入配置或模板路径。
这不是设备实时流测试，也不代替生产 DMA/NV12 验收。

| 录像与帧 | 基板输出 | 门输出 | 结论 |
|---|---|---|---|
| 23:24:27 录像，190 s，无板有码 | 1 / `QrFound`，分数 1.000 | 2 / `Clear` | 符合样本标签 |
| 23:32:18 录像，150 s，有板稳态 | 2 / `QrAbsentSceneOk`，分数 0.221 | 2 / `Clear` | 符合样本标签 |
| 第一段录像，100 s，六灯开、近黑 | 0 / `Unknown` | 1 / `Occluded`，Y 均值 16.128 | 黑画面不被当作有板 |
| 第一段录像，83.5 s，局部门遮挡 | **2 / `QrAbsentSceneOk`，分数 0.146** | 2 / `Clear` | **已知失败：不能证明基板存在** |

最后一行保留为失败，不纳入“现场全部通过”的结论。此模块不提供关门到位反馈；
SOC 必须在机构停稳且工作区域可见后才请求基板检测。粉尘／工具挡码、相机偏移等
负样本还需要实机验收，不能仅靠软件测试或这四张抽帧批准量产。

## 交给 SOC 的后续事项

1. 评审本分支的会话／回复等待修正和当前 SOC/UI 流程，核对同版本契约与超时／终止处理。
2. 不在真实 Broker 上新开相同身份的测试客户端。实际业务入口已接入且已进行部分现场测试，完整实机验收仍需留存记录。
3. 确认灯、机构和图像准备条件；验证内置同型号候选参数，正常测试不要求操作人员逐台部署文件。
4. 后续部署仍须核对 gsx 的 UPDF `fd_count`、应用 A/B 升级机制、设备版本与回退方案；历史测试包的部署不等于新版本自动获准。
5. 依据 [PLATE_FULLFLOW_TEST.md](PLATE_FULLFLOW_TEST.md) 验收。外门弹窗 Cancel 是跳过外门检查，不是终止；基板通过后可能进入实际打印；基板开关也不自动启用测高。

上表所述 2026-09-24 软件测试轮次没有变更机器程序、灯状态、门状态、Z 位置或打印状态；后续部署与现场操作不属于该轮次。

## 本轮软件集成修正

- 门检测与基板检测共用一个递增 session 编号，避免各从 1 开始导致后一类请求被 Visual 当成旧会话拒绝。
- SOC 在发送请求前建立等待状态，避免快速回复在等待前到达后被丢弃；首个有效结果固定，迟到／重复回复不覆盖。
- Visual 回复校验身份、类型、精确长度、编号和保留字段；放板确认包损坏不再被当作 Confirm。
- 窗口超时、发送失败、终止仍保守地返回 Unknown／取消，不新增机构动作或改变开关默认值。

参数 ID：`station_172_17_101_78_zminus20_candidate_v1`，内容 SHA256：`1f1139ce2e94f2582bce86e96d7d2ce9f38ff146c3e1042540979aa54a1c4b05`。原工作树的本地修改不移动、不删除；实机测试完成后才考虑清理临时分支。

## 可复现命令与产物

在 WSL Ubuntu 中：

```sh
cmake -S /mnt/c/Users/A/upc-plate-test/app/glavo/tests -B /tmp/upc-plate-soc-exchange-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/upc-plate-soc-exchange-tests --parallel 6
ctest --test-dir /tmp/upc-plate-soc-exchange-tests --output-on-failure
BUILD_DIR=/tmp/upc-plate-fullflow-cross OUTPUT_DIR='/mnt/d/Base Plate Inspection/output/plate-fullflow-20260924-test1' JOBS=6 bash /mnt/c/Users/A/upc-plate-test/build.sh
```

Visual 五项测试命令见 `app/visual/tests/README.md`。以上命令和包哈希用于复现历史测试提交 `b54e792`；合入 `gsx` 的源码已恢复原产品版号。已有 CMake 缓存的产品版本不得与根 CMakeLists 不一致。运行 build.sh 前核对独立 OUTPUT_DIR，它会重建其 install/app_ota_stage 子目录。

测试包：`D:\Base Plate Inspection\output\plate-fullflow-20260924-test1\soc_app_ota_V4.2.0.2-plate-test1.tar.gz`，3,841,817 字节。

包 SHA256：`dfe7bd028d0bc7e11880635754802c1dc773d2e8197a1bc60ba65d4384574926`。同目录的 `.sha256` 用于传输检查。新包与之前基于 01f27d3 的 `gsx-builtin-20260924-01` 算法侧包不同，不可混淆。

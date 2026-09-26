# 舱检算法与 IPC 隔离测试

本测试属于 `gsx` 算法侧交付。协议及 SOC 调用方法见
[CABIN_IPC_INTEGRATION.md](../../../doc/CABIN_IPC_INTEGRATION.md)。

## 在 Linux / WSL Ubuntu 上运行

需要 CMake、C++17 编译器、构建机 Python 3（仅标准库）和 OpenCV 的 core/imgproc/imgcodecs 开发库。设备不需要 Python。
以下目录是本次 Windows 工作区在 WSL 下的路径；其他机器替换 `-S` 路径即可。

```sh
cmake -S /mnt/c/Users/A/upc-plate-test/app/visual/tests -B /tmp/upc-plate-fullflow-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/upc-plate-fullflow-tests --parallel 6
ctest --test-dir /tmp/upc-plate-fullflow-tests --output-on-failure
```

五个 CTest 目标：

1. `cabin_contract_tests`：六个消息的数值、可靠投递登记、POD 大小与字节布局。
2. `cabin_detector_tests`：输入／标定异常、近黑／非近黑、合成标记、质量判断、新帧连续证据、序号与重新加载失败。
3. `cabin_ipc_test`：独立 Broker、真实 VisualService、模拟 Camera 三个子进程；父进程只模拟 Glavo 收发。真实通过 MsgPort、Unix socket、SCM_RIGHTS 文件描述符和带 stride 的 SHM 元数据环，不把算法返回值冒充消息。
4. `cabin_profile_tests`：无文件自动使用内置参数、覆盖优先级、坏配置／断链不回退、运行时不提取或写出参数。
5. `cabin_bundled_profile_tests`：只用编进程序的 PNG 合成有码图，验证内置正样本、黑帧拒绝、灯状态和失效重载。

集成测试覆盖：

- 有码回复后停止，无新命令不自动重新检测／取流；新命令才能复检。
- WaitOpen 忽略清晰帧，稳定近黑才回复；清晰帧一直存在则超时 Unknown。
- 黑帧不能通过基板在位检测；无新帧、所选工程配置无效、灯状态错误返回 Unknown。
- 重复请求、旧请求、错误 session、保留字段、非法模式、错误长度。
- 完成结果缓存重发、同编号改参数、并发请求拒绝。
- Stop 取消后不发迟到结果；旧 Stop 不结束新会话。
- 暂停周期结果投递时取消，恢复后也不发旧结果；只 Start 不检测的会话在 15 秒后释放流。
- 原有光路会话优先，舱检不能拆掉仍需要的原有流。
- 主机无 `/userdata/visual/cabin_*_config.json` 时，清除测试子进程的配置环境变量，经过实际 IPC 验证内置门参数及黑帧基板 Unknown；若主机已有工程覆盖则明确打印跳过，不动这些文件。

测试会为自己创建 `/tmp/upc-cabin-ipc-XXXXXX/broker.sock`、独立 SHM 和临时配置，
结束时清理自己的文件。不读取生产 Broker 地址，也不启动机器 Camera、Glavo 或任何机构。
**不要把测试客户端改为连接 `/tmp/upc_broker.sock`**：同一 ModuleId 会挤掉真实服务连接。

## 交叉编译（不部署）

仓库现有 phx 工具链路径配置在 `cmake/phx-buildroot-toolchain.cmake`。
工具链及 sysroot 已准备好时：

```sh
cmake -S /mnt/c/Users/A/upc-plate-test -B /tmp/upc-plate-fullflow-cross -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/upc-plate-fullflow-cross --target rongsu_visual rongsu_broker --parallel 6
```

这只产生 aarch64 Linux 程序，不复制程序到设备，不重启服务。真实部署必须先核对
gsx 的 UPDF `fd_count` 传输版本，不能混用旧工作区的二进制。

## 内置参数与交付检查

编译时把候选门参数、基板参数和五张 PNG 模板嵌入 Visual，不依赖目标机器源码目录或外部模板路径。新安装且没有工程覆盖的机器使用内置参数；测试与工程人员仍可显式选择外部配置。优先级及无效覆盖的 Unknown 规则见 [calibration/README.md](../calibration/README.md)。测试缺失配置时必须区分“没有覆盖，使用内置参数”和“明确指定了不存在的配置，失败”，不能把两者都判作缺标定。

交付验证还应检查：无外部文件启动、两类工程覆盖独立选择、无效覆盖不偷偷回退，以及内置模板确实来自编译产物而非开发机文件。嵌入不会改变六条 IPC 消息、会话开始／停止规则和既有视觉任务的优先级。实际测试结果和边界记录在 [CABIN_TEST_REPORT.md](../../../doc/CABIN_TEST_REPORT.md)，本说明不代表上机验收通过。

离线图像可直接用内置参数复核，无须提供 JSON 路径：`cabin_frame_check --bundled IMAGE...`。旧的 `cabin_frame_check PLATE_CONFIG DOOR_CONFIG IMAGE...` 显式覆盖模式保留。

`gsx` 的全量 `build.sh` 继续生成原有 `soc_app_ota_*.tar.gz` 和校验文件；包内无须增加舱检 JSON／模板。不要把仅编译 Visual/Broker 的上述命令当作完整升级包，也不要为了补参数单独替换与生产传输版本不兼容的 Visual。`dtx` 不具备此版本的应用包流程，不能使用 `gsx` 应用包代替它的现有部署方式。

## 验证边界

自动测试通过不等于现场验收。当前基板算法是标定模板判别，不是通用二维码解码；
已有局部门遮挡被判断为无码的失败样本仍然是已知风险。需要 SOC 保证检测时机，
并补生产 NV12、灰尘／反光／遮挡／偏移和机构动作的实机验收。
候选参数现可作为无外部覆盖时的内置默认值；这消除操作人员手工管理文件的步骤，不证明跨机器检测已通过验收。本分支已包含 Glavo/UI 起印闸门与业务处理；这些隔离测试仍只覆盖 Visual 接收侧，无请求仍不检测。

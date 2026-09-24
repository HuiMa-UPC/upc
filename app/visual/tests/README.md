# 舱检算法与 IPC 隔离测试

本测试属于 `dtx` 算法侧交付（从 `gsx` 的舱检模块移植，保留 `dtx` 原有传输与会话）。协议及 SOC 调用方法见
[CABIN_IPC_INTEGRATION.md](../../../doc/CABIN_IPC_INTEGRATION.md)。

## 在 Linux / WSL Ubuntu 上运行

需要 CMake、C++17 编译器和 OpenCV 的 core/imgproc/imgcodecs 开发库。
以下目录是本次 Windows 工作区在 WSL 下的路径；其他机器替换 `-S` 路径即可。

```sh
cmake -S /mnt/c/Users/A/upc-dtx-cabin/app/visual/tests -B /tmp/upc-dtx-cabin-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/upc-dtx-cabin-tests --parallel 6
ctest --test-dir /tmp/upc-dtx-cabin-tests --output-on-failure
```

三个 CTest 目标：

1. `cabin_contract_tests`：六个消息的数值、可靠投递登记、POD 大小与字节布局。
2. `cabin_detector_tests`：输入／标定异常、近黑／非近黑、合成标记、质量判断、新帧连续证据、序号与重新加载失败。
3. `cabin_ipc_test`：独立 Broker、真实 VisualService、模拟 Camera 三个子进程；父进程只模拟 Glavo 收发。真实通过 MsgPort、Unix socket、SCM_RIGHTS 文件描述符和带 stride 的 SHM 元数据环，不把算法返回值冒充消息。

集成测试覆盖：

- 有码回复后停止，无新命令不自动重新检测／取流；新命令才能复检。
- WaitOpen 忽略清晰帧，稳定近黑才回复；清晰帧一直存在则超时 Unknown。
- 黑帧不能通过基板在位检测；无新帧、无标定、灯状态错误返回 Unknown。
- 重复请求、旧请求、错误 session、保留字段、非法模式、错误长度。
- 完成结果缓存重发、同编号改参数、并发请求拒绝。
- Stop 取消后不发迟到结果；旧 Stop 不结束新会话。
- 暂停周期结果投递时取消，恢复后也不发旧结果；只 Start 不检测的会话在 15 秒后释放流。
- 原有光路会话优先，舱检不能拆掉仍需要的原有流。

测试会为自己创建 `/tmp/upc-cabin-ipc-XXXXXX/broker.sock`、独立 SHM 和临时配置，
结束时清理自己的文件。不读取生产 Broker 地址，也不启动机器 Camera、Glavo 或任何机构。
**不要把测试客户端改为连接 `/tmp/upc_broker.sock`**：同一 ModuleId 会挤掉真实服务连接。

## 交叉编译（不部署）

仓库现有 phx 工具链路径配置在 `cmake/phx-buildroot-toolchain.cmake`。
工具链及 sysroot 已准备好时：

```sh
cmake -S /mnt/c/Users/A/upc-dtx-cabin -B /tmp/upc-dtx-cabin-cross -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/upc-dtx-cabin-cross --target rongsu_visual rongsu_broker --parallel 6
```

这只产生 aarch64 Linux 程序，不复制程序到设备，不重启服务。本移植保留 `dtx`
原有 UPDF 传输（8 字节包头），没有引入 `gsx` 的 `fd_count` 字段（12 字节包头）。
真实部署必须先核对机器使用的分支与传输版本；不能把本分支二进制与 `gsx` 新传输版本混用。
测试用的 Broker、VisualService 和模拟 Camera 均来自本 `dtx` 工作区。

## 验证边界

自动测试通过不等于现场验收。当前基板算法是标定模板判别，不是通用二维码解码；
已有局部门遮挡被判断为无码的失败样本仍然是已知风险。需要 SOC 保证检测时机，
并补生产 NV12、灰尘／反光／遮挡／偏移和机构动作的实机验收。
候选配置没有自动启用，Glavo/UI 发送和业务处理尚未接入。

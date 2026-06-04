# services 模块 — Agent 使用指南

> 命名空间：`services`  
> CMake 目标：`services::port`  
> 依赖：[`middleware/AGENTS.md`](../middleware/AGENTS.md)  
> UI 协作：[`ui/AGENTS.md`](../ui/AGENTS.md)  
> 协议开发者：[`glavo/parsers/README.md`](glavo/parsers/README.md)

---

## 1. 模块概览

| 区域 | 职责 |
|------|------|
| `service_base.hpp/.cpp` | `ServiceBase` 生命周期、MsgBus 订阅/发布、可选独立工作线程 |
| `parser/` | 解析框架：`IParserWorker`、`ParserRegistry`、`ParserManager`、`ParserContext` |
| `glavo/` | 振镜打印后端：`GlavoService` 编排、TCP/串口下发、MCU 协议、RingBuffer |
| `glavo/parsers/<协议>/` | 各切片格式的解析实现（编译期插件，见 README） |

生产链路：

```
UI (MsgBus)
  → GlavoService（独立工作线程）
  → ParserManager → IParserWorker（如 GcodeWorker）
  → RingBuffer
  → TcpSender → protocol_mcu → 下位机
```

---

## 2. 目录结构

```
services/
├── CMakeLists.txt
├── AGENTS.md
├── service_base.hpp / service_base.cpp
├── parser/                              # 解析框架（稳定，主工程维护）
│   ├── parser_worker.hpp                # IParserWorker 接口
│   ├── parser_registry.hpp / .cpp       # 扩展名注册与路由
│   ├── parser_manager.hpp / .cpp        # 当前活跃解析器管理
│   └── parser_context.hpp / .cpp        # 启动时加载的共享校准数据
└── glavo/
    ├── glavo_service.hpp / .cpp         # 文件目录、打印流程、进度上报
    ├── tcp_sender.hpp / .cpp            # RingBuffer 消费、TCP/串口链路
    ├── ring_buffer.hpp / .cpp           # 解析输出队列（统一契约）
    ├── protocol_mcu.hpp / .cpp          # MCU 帧组包/解包
    ├── cal_parser.hpp / .cpp            # CSV 振镜校准文件解析
    ├── print_job_state.hpp / .cpp
    ├── device_config.hpp / gcode_config.hpp
    └── parsers/
        ├── README.md                    # 协议开发者指南
        └── gcode/                       # G-code 解析（参考实现）
            ├── gcode_worker.hpp / .cpp
            └── gcode_register.cpp
```

常用 include：

```cpp
#include "service_base.hpp"
#include "glavo_service.hpp"
#include "parser_manager.hpp"
#include "parser_registry.hpp"
```

---

## 3. CMake 集成

```cmake
add_subdirectory(services)
target_link_libraries(rongsu_ui PRIVATE services::port middleware::port)
```

源文件通过 GLOB 自动收录，**通常无需改 CMakeLists**：

| 路径 | 说明 |
|------|------|
| `services/*.c*` | ServiceBase 等 |
| `services/parser/*.c*` | 解析框架 |
| `services/glavo/*.c*` | Glavo 后端 |
| `services/glavo/parsers/*/*.c*` | 各协议插件 → 独立库 `services_parsers` |

根 `CMakeLists.txt` 对 `services_parsers` 使用 `-Wl,--whole-archive`，确保 `*_register.cpp` 静态注册在启动时执行。

编译选项：

| 选项 | 说明 |
|------|------|
| `-DSERVICES_DEBUG=ON` | GcodeWorker / GlavoService 调试日志 |
| `-DTCP_DEBUG=ON/OFF` | TcpSender 链路调试日志 |

---

## 4. 解析框架

### 4.1 组件职责

| 类 | 职责 |
|----|------|
| `IParserWorker` | 解析器抽象接口（open/start/stop/进度/RingBuffer） |
| `ParserRegistry` | 编译期静态注册；按文件后缀（小写）路由到解析器 |
| `ParserManager` | 同一时刻只管理一个活跃解析器；对接 TcpSender |
| `ParserContext` | 启动时 `load_calibration()`，各解析器按需读取 |

### 4.2 注册与插拔

每个协议在 `parsers/<name>/` 下提供 `*_register.cpp`，通过静态构造注册：

```cpp
ParserRegistry::instance().register_parser({
    .id = "gcode",
    .extensions = {".gcode", ".nc"},
    .factory = []() -> IParserWorker* {
        return &GcodeWorker::instance();
    },
});
```

- **放入目录 + 注册文件 → 编译后生效**
- **删除整个协议目录 → 该后缀不再出现在文件列表，工程仍可编译**
- **核心代码（GlavoService / TcpSender）不得 `#include` 具体协议头文件**

### 4.3 目录扫描

`GlavoService::handle_dir_open()`：

1. 若 `ParserRegistry::has_any()` 为 false → 返回空列表
2. 扫描磁盘目录（默认 `/home/Gcode`）
3. 仅保留后缀匹配 `ParserRegistry::all_extensions()` 的文件（**统一转小写比较**）
4. 通过 `Fs_DirOpen_Rsp` 返回给 UI

### 4.4 初始化顺序（`app/src/main.cpp`）

```cpp
services::GlavoService glavo_service;
glavo_service.init();   // ParserContext 加载校准 → bind_tcp_output → TcpSender 回调
// ...
glavo_service.start();  // 订阅 MsgBus
```

`GlavoService::do_init()` 内：

```cpp
ParserContext::instance().load_calibration(CAL_DEFAULT_FILE_PATH);
ParserManager::instance().bind_tcp_output();
```

---

## 5. GlavoService 与打印流程

| 消息 | 处理 |
|------|------|
| `Fs_DirOpen_Req` | 扫描目录，过滤已注册后缀 |
| `UI_FileInfo_Req` | `ParserManager::open_file` + `start_parse`，返回行数/大小 |
| `UI_Print_Start_Req` | 确认打印，`TcpSender::begin_sending()` |
| `UI_Print_Terminate_Req` | `ParserManager::stop_parse` + `TcpSender::request_terminate` |
| `APP_Print_Progress_Ntf` | 读取活跃解析器进度 + PrintJobState |

**禁止**在 GlavoService / TcpSender 中直接调用 `GcodeWorker::instance()`，统一走 `ParserManager::instance().active()`。

---

## 6. ServiceBase 生命周期

```
init()  → do_init()
start() → do_start()   // subscribe；use_worker_thread=true 时启动工作线程
stop()  → do_stop()
```

| 方法 | 说明 |
|------|------|
| `subscribe(type, handler)` | 订阅消息，handler 内调用 `on_message(msg)` |
| `publish(msg)` | 同步分发（慎用） |
| `publish_async(msg)` | 异步入队，由 `MsgBus::process()` 分发（**回复 UI 必须用此**） |

### 线程安全红线

`GlavoService` 已开启 `use_worker_thread = true`：

- `handle_message()` 在工作线程执行，**禁止**直接操作 LVGL 对象
- 回复 UI **必须** `publish_async()`，**禁止** `publish()`
- 解析器在自有线程中写 RingBuffer；TcpSender 在链路线程中读，通过 RingBuffer 内部锁同步

---

## 7. 添加新后端 Service

1. 在 `middleware/message_bus/msg_types.hpp` 增加 `ModuleId` / `MsgType`
2. 继承 `ServiceBase`，实现 `do_init` / `do_start` / `handle_message`
3. 在 `app/src/main.cpp` 实例化、`init()`、`start()`
4. 在 `UiMsgProxy` 封装 send/register

---

## 8. 添加新切片格式

详见 [`glavo/parsers/README.md`](glavo/parsers/README.md)。概要：

1. 在 `glavo/parsers/<协议名>/` 新建 worker（实现 `IParserWorker`）
2. 新建 `<协议名>_register.cpp` 注册后缀
3. 解析结果写入统一 `RingBuffer`（`RB_PKT_MOTION` / `RB_PKT_LAYER_MARK`）
4. 按需从 `ParserContext` 读取校准；不需要则可忽略

---

## 9. Agent 快速检查清单

- [ ] GlavoService / TcpSender 不直接依赖具体协议头文件
- [ ] 新协议通过 `ParserRegistry` 注册，后缀小写
- [ ] 回复 UI 用 `publish_async` + `target_id = ModuleId::UI`
- [ ] 解析线程写 RingBuffer 时用 `put_wait(pkt, stop_flag)`，支持 stop 唤醒
- [ ] 删除某协议目录后，`ParserRegistry::has_any()` 为 false 时目录列表为空
- [ ] 修改 `parser/` 框架时同步更新 `glavo/parsers/README.md`

---

## 10. 相关文档

| 文档 | 内容 |
|------|------|
| [middleware/AGENTS.md](../middleware/AGENTS.md) | MsgBus、MsgType、BufferPool |
| [ui/AGENTS.md](../ui/AGENTS.md) | control、UiMsgProxy |
| [glavo/parsers/README.md](glavo/parsers/README.md) | 协议开发者：接口、输出格式、校准 |
| `app/src/main.cpp` | GlavoService / TcpSender 集成 |

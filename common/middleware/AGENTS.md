# middleware 模块 — Agent 使用指南

> 本文档供 AI Agent / 开发者快速理解并集成 `middleware` 静态库。  
> 命名空间：`middleware`  
> CMake 目标：`middleware::port`（别名，实际库名为 `middleware`）

---

## 1. 模块概览

| 子模块 | 路径 | 职责 |
|--------|------|------|
| **message_bus** | `message_bus/` | 发布/订阅消息总线，UI 线程 `process()` 消费异步队列 |
| **memory_pool** | `memory_pool/` | 固定块内存池 + 引用计数缓冲区池 |
| **tread_pool** | `tread_pool/` | 工作线程池（目录名拼写为 tread_pool，非 thread_pool） |
| **debug** | `debug/` | 可宏开关的 stderr 调试日志（`MW_LOG_*`、`mw_debug_dump_*`） |

均为 C++17，无 LVGL 依赖，可独立链接到任意业务模块。

---

## 2. 目录结构

```
middleware/
├── CMakeLists.txt
├── AGENTS.md
├── message_bus/
│   ├── msg_types.hpp     # Message / MsgType / ModuleId / MsgPayload
│   ├── msg_bus.hpp       # MsgBus 单例、Subscription
│   ├── msg_bus.cpp
│   ├── msg_queue.hpp     # PriorityMsgQueue（MsgBus 异步队列）
│   ├── msg_queue.cpp
│   ├── ring_buffer.hpp   # 字节环形缓冲（独立工具类）
│   └── ring_buffer.cpp
├── memory_pool/
│   ├── mem_pool.hpp
│   ├── mem_pool.cpp
│   ├── buffer_pool.hpp
│   └── buffer_pool.cpp
├── tread_pool/
│   ├── thread_pool.hpp
│   └── thread_pool.cpp
└── debug/
    ├── mw_debug.hpp      # 调试宏 MW_LOG_* 与状态 dump API
    └── mw_debug.cpp
```

**include 方式**（CMake 已注入各子目录）：

```cpp
#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"
#include "thread_pool.hpp"
#include "mw_debug.hpp"
```

---

## 3. CMake 集成

### 3.1 作为子工程（推荐）

在根 `CMakeLists.txt` 中：

```cmake
add_subdirectory(middleware)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE middleware::port)
```

链接后自动获得：

- 头文件路径：`message_bus/`、`memory_pool/`、`tread_pool/`、`debug/`（与各 `.hpp` 同目录）
- 编译宏：`MSG_BUS_QUEUE_DEPTH`、`MEM_POOL_MAX_BUFFERS`、`THREAD_POOL_MAX_THREADS`、`MIDDLEWARE_DEBUG` 等
- Linux 下 `Threads::Threads`（pthread）

### 3.2 调试日志（`debug/mw_debug.hpp`）

#### 总开关

| CMake 选项 | 默认 | 说明 |
|------------|------|------|
| `MIDDLEWARE_DEBUG` | OFF | 总开关；OFF 时所有 `MW_LOG_*` 宏为空，**零运行时开销** |
| `MIDDLEWARE_DEBUG_VERBOSE_POOL` | OFF | 仅当 `MIDDLEWARE_DEBUG=ON` 时有效；开启 MemPool/BufferPool **逐条** alloc/free 日志 |

```bash
# 推荐：仅前后端消息流（默认子开关见下表）
cmake -B build -DMIDDLEWARE_DEBUG=ON
cmake --build build

# 需要内存池/缓冲池逐条日志（刷屏，慎用）
cmake -B build -DMIDDLEWARE_DEBUG=ON -DMIDDLEWARE_DEBUG_VERBOSE_POOL=ON
```

关闭：`-DMIDDLEWARE_DEBUG=OFF` 或不传该选项。

#### 子开关默认值（`MIDDLEWARE_DEBUG=1` 时）

| 宏 | 默认 | 标签 | 内容 |
|----|------|------|------|
| `MW_DEBUG_MSGBUS` | **1** | `[MW\|MSGBUS]` | ENQUEUE / DELIVER / PUBLISH、subscribe/unsubscribe、process 汇总 |
| `MW_DEBUG_MEMPOOL` | **0** | `[MW\|MEM]` | MemPool alloc/free、各档位 used/total |
| `MW_DEBUG_BUFFERPOOL` | **0** | `[MW\|BUF]` | BufferPool alloc/acquire/release |
| `MW_DEBUG_THREADPOOL` | **1** | `[MW\|POOL]` | ThreadPool init/submit/shutdown |

CMake 开启 `MIDDLEWARE_DEBUG_VERBOSE_POOL` 时会注入 `MW_DEBUG_MEMPOOL=1` 与 `MW_DEBUG_BUFFERPOOL=1`。也可在 `#include "mw_debug.hpp"` 前手动 `#define`。

#### 日志输出示例

```
[MW|MSGBUS] init ok queue_depth=256
[MW|MSGBUS] subscribe id=1 type=DemoHelloRsp module=UI total=1
[MW|MSGBUS] ENQUEUE DemoHelloReq UI->APP pri=1 queue=1 payload={inline(0)}
[MW|MSGBUS] process delivered=1
[MW|MSGBUS] DELIVER DemoHelloReq UI->APP pri=1 handlers=1 payload={inline(0)}
[MW|MSGBUS] ENQUEUE DemoHelloRsp APP->UI pri=1 queue=1 payload={inline(10)="HelloWorld"}
```

文本 payload 会自动截断至 48 字符；二进制 Inline 显示为 `inline(N,bin)`。

#### 运行时状态 dump

```cpp
#include "mw_debug.hpp"

middleware::mw_debug_dump_status();              // MsgBus 队列 + 订阅数 + MemPool/BufferPool 汇总
middleware::mw_debug_dump_thread_pool(&pool);    // 指定 ThreadPool 实例
```

辅助 API（供 middleware 内部或扩展使用）：

```cpp
mw_debug_module_name(ModuleId id);
mw_debug_msgtype_name(MsgType type);
mw_debug_payload_summary(payload, buf, buf_size);
mw_debug_log_message("ENQUEUE", msg, /*handlers=*/0, /*queue_depth=*/1);
```

#### 实现约束（修改 debug 代码前必读）

1. **禁止在持锁期间**调用 `fprintf` / `MW_LOG_*`，也**禁止**在持锁期间调用 `pending_count()`、`is_full()`、`subscriber_count()` 等可能再次加锁的 MsgBus API（`std::mutex` 不可重入，会导致 UI 线程死锁、触摸无响应）。
2. `publish_async`：先在锁内 `push` 并记录 `queue_depth`，**释放锁后**再 `mw_debug_log_message(..., queue_depth)`。
3. MemPool / BufferPool：在锁内完成 alloc/free，**解锁后**再打日志。
4. 默认关闭 MEM/BUF 逐条日志，是为避免 stderr 阻塞拖慢嵌入式 UI 主循环；需要时可开 `MIDDLEWARE_DEBUG_VERBOSE_POOL`。

### 3.3 覆盖配置宏

```bash
cmake -B build -DMSG_BUS_QUEUE_DEPTH=512 -DTHREAD_POOL_MAX_THREADS=8
```

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `MSG_BUS_QUEUE_DEPTH` | 256 | 异步消息队列深度 |
| `MEM_POOL_MAX_BUFFERS` | 128 | BufferPool 最大缓冲条数 |
| `THREAD_POOL_MAX_THREADS` | 4 | 线程池默认线程数 |
| `THREAD_POOL_QUEUE_DEPTH` | 64 | 普通任务队列深度 |
| `MIDDLEWARE_DEBUG` | OFF | CMake option，调试日志总开关 |
| `MIDDLEWARE_DEBUG_VERBOSE_POOL` | OFF | 开启 MemPool/BufferPool 逐条调试日志 |

### 3.4 独立构建（调试 middleware）

```bash
cd middleware
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DMIDDLEWARE_DEBUG=ON
cmake --build build
# 产物：build/libmiddleware.a（或 .lib）
```

---

## 4. 推荐初始化顺序

在 `main()` 或应用启动阶段：

```cpp
#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"
// thread_pool 按需创建实例，非单例

bool app_init() {
    if (!middleware::MemPool::instance().init()) return false;
    if (!middleware::BufferPool::instance().init()) return false;
    if (!middleware::MsgBus::instance().init()) return false;
    return true;
}

void app_shutdown() {
    middleware::MsgBus::instance().deinit();
    middleware::BufferPool::instance().deinit();
    middleware::MemPool::instance().deinit();
}
```

---

## 5. message_bus 使用说明

### 5.1 核心类型

当前仓库 **Demo 精简版** 枚举（`message_bus/msg_types.hpp`）：

```cpp
enum class ModuleId : uint32_t {
    UI  = 0x01,
    APP = 0x02,
};

enum class MsgType : uint32_t {
    DemoHelloReq = 0x1000,
    DemoHelloRsp,
};
```

- **`Message`**：`type` + `sender_id` + `target_id` + `payload`
- **`MsgPayload`**：内联字节 / 缓冲区引用 / 数值 / 指针（Demo 使用 Inline 字符串）

扩展新业务能力时，在 `msg_types.hpp` 追加 `ModuleId` / `MsgType`（见 §5.5）。

### 5.2 订阅（RAII，析构自动取消）

```cpp
#include "msg_bus.hpp"

using namespace middleware;

class MyService {
    Subscription sub_;
public:
    void start() {
        sub_ = MsgBus::instance().subscribe(
            MsgType::DemoHelloReq,
            ModuleId::APP,
            [](const Message& msg) {
                (void)msg;
                // 处理后 publish_async DemoHelloRsp → UI
            });
    }
};
```

- `subscribe_all(module_id, handler)`：`MsgType{0}` 表示订阅所有类型
- `target_id == ModuleId{0}` 表示广播

### 5.3 发布与 UI 线程消费

```cpp
// 异步：UI 发 DemoHelloReq
Message req(MsgType::DemoHelloReq, ModuleId::UI, ModuleId::APP);
MsgBus::instance().publish_async(req);

// 主循环消费
MsgBus::instance().process();
```

**重要**：

- `publish_async` 的消息必须由 **`process()`** 在目标线程（通常为 UI 线程）取出并分发。
- 异步队列已接入 **`PriorityMsgQueue`**，按 `Message::priority` 降序出队（同优先级 FIFO）。
- `publish_async` 队列满返回 `false` 时，若 payload 为 **`BufferRef`**，MsgBus 会自动 `BufferPool::release`，避免泄漏。
- `deliver()` 在**锁外**调用 handler，handler 内可安全 `subscribe` / `publish_async`（仍避免在 handler 里同步 `publish` 递归过深）。
- `deinit()` 会排空异步队列并对其中 `BufferRef` 执行 `release`。

#### 状态查询

```cpp
MsgBus::instance().pending_count();      // 异步队列中待 process 的消息数
MsgBus::instance().is_full();            // 队列是否已满（≥ MSG_BUS_QUEUE_DEPTH）
MsgBus::instance().subscriber_count();   // 当前订阅条目数
```

> 上述 API 会加锁；**勿在已持有 `queue_mutex_` / `subscribers_mutex_` 的代码路径内调用**（debug 日志亦同）。

### 5.4 RingBuffer

独立工具类，用于原始字节流，与 MsgBus 无耦合：

```cpp
middleware::RingBuffer rb(4096);
rb.write(data, len);
rb.read(buf, sizeof(buf));
```

### 5.5 模块 ID 与消息类型：如何添加 / 删除

**唯一入口文件**：`message_bus/msg_types.hpp`  
`MsgBus` 本身不维护模块或消息类型的注册表；改枚举 + 全仓更新引用即可。

#### 编号约定（请勿随意重排已有数值）

| 范围 | 用途 | 示例 |
|------|------|------|
| `0x01` ~ `0x0F` | 核心/UI | `UI`, `APP` |
| `0x10` ~ `0x1F` | 设备/驱动服务 | `ETH`, `UART`, `WIFI`, `CAMERA`, `FS` |
| `0x20` ~ `0xFF` | 预留业务模块 | 按需扩展 |
| `0x100` 步进 | 消息类型分段 | 网络 `0x100`、串口 `0x200`、WiFi `0x300`… |
| `≥ 0x1000` | 用户自定义 | `ModuleId::USER`、`MsgType::UserBase` 起 |

> **注意**：枚举底层是 `uint32_t`，已发布/持久化/跨进程使用的数值 **不要改、不要复用**，删除时用「废弃」而非改号。

---

#### 添加新模块（ModuleId）

1. 编辑 `msg_types.hpp` 的 `enum class ModuleId`，分配未占用的十六进制值：

```cpp
enum class ModuleId : uint32_t {
    // ... 已有项 ...
    BLUETOOTH = 0x15,   // 新驱动模块，放在 0x10~0x1F 段
    SETTINGS  = 0x21,   // 新业务模块，放在 0x20~ 段
};
```

2. 在新模块代码里发布/订阅时使用该 ID：

```cpp
// 蓝牙服务订阅
MsgBus::instance().subscribe(MsgType::UserBase, ModuleId::BLUETOOTH, handler);

// 发给 UI
Message m(MsgType::UserBase, ModuleId::BLUETOOTH, ModuleId::UI);
```

3. **无需**改 `msg_bus.cpp`、`CMakeLists.txt`（除非新建独立源码目录，见下文「添加 middleware 子目录」）。

---

#### 添加新消息类型（MsgType）

**方式 A — 已有分段内追加**（如再增加一条网络消息）：

```cpp
enum class MsgType : uint32_t {
    // 网络 (0x100)
    NetConnectReq   = 0x100,
    // ...
    NetDataReceived,
    NetPingReq,         // 自动递增，紧接在同段末尾
};
```

**方式 B — 新分段**（新子系统，如蓝牙 `0x600`）：

```cpp
enum class MsgType : uint32_t {
    // ... 已有分段 ...
    FsOperationDone,

    // 蓝牙 (0x600)
    BtScanReq       = 0x600,
    BtConnectReq,
    BtDataReceived,

    UserBase        = 0x1000   // 保持 UserBase 在自定义段起点
};
```

**方式 C — 仅应用层自定义**（不改公共枚举，适合局部功能）：

```cpp
constexpr MsgType MSG_PAGE_SWITCH =
    static_cast<MsgType>(static_cast<uint32_t>(MsgType::UserBase) + 1);

Message m(MSG_PAGE_SWITCH, ModuleId::APP, ModuleId::UI);
```

添加后：在发送方 `publish` / `publish_async`，在接收方 `subscribe(新类型, module_id, handler)`。

---

#### 删除不需要的模块 / 消息类型

1. **在 `msg_types.hpp` 中删除或标记废弃**（推荐保留注释说明已废弃及原值，避免后人复用编号）：

```cpp
// DEPRECATED: ModuleId::WIFI = 0x12 — 已移除 WiFi 模块，勿复用
enum class ModuleId : uint32_t {
    UI = 0x01,
    ETH = 0x10,
    // WIFI 已删除
};
```

2. **全仓搜索并清理引用**（在仓库根目录执行）：

```bash
rg "ModuleId::WIFI|MsgType::Wifi" --glob '*.{cpp,hpp,c,h}'
```

3. 删除对应业务目录（如 `services/wifi/`）及 CMake 中的 `add_subdirectory` / 源文件列表。

4. **不要**只删枚举而不删订阅/发布代码，否则编译报错；若需临时保留编译，可先改为 `UserBase` 段自定义类型再逐步迁移。

| 场景 | 建议 |
|------|------|
| 功能尚未上线 | 直接删枚举 + 删代码 |
| 已上线 / 有日志或持久化 | 保留枚举值，注释 `// deprecated`，业务层不再使用 |
| 消息跨进程/存盘 | **禁止**更改已有 `MsgType` / `ModuleId` 数值 |

---

#### 添加 middleware 新子目录（如 `protocol/`）

与 `message_bus`、`memory_pool` 同级的新 **C++ 库** 需改 CMake：

1. 创建目录，例如 `middleware/protocol/`，放置 `xxx.hpp` / `xxx.cpp`。
2. 在 `middleware/CMakeLists.txt` 的 `MIDDLEWARE_SOURCES` 追加 `.cpp`。
3. 在 `target_include_directories` 增加 `.../protocol`。
4. 更新本文件 §2 目录树说明。

```cmake
list(APPEND MIDDLEWARE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol/xxx.cpp
)
target_include_directories(${MIDDLEWARE_TARGET} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/protocol>
    # ... 已有路径 ...
)
```

#### 删除 middleware 子目录

1. 从 `middleware/CMakeLists.txt` 移除对应源文件与 include 路径。
2. 删除目录。
3. 全仓 `rg` 搜索该子目录头文件名并移除 `#include` 与链接依赖。
4. 根工程 `target_link_libraries(... middleware::port)` **不必改**（仍链接整个 middleware 静态库）。

---

#### Agent 操作检查清单（扩展/裁剪后）

- [ ] 新 `ModuleId` / `MsgType` 数值未与现有段冲突
- [ ] `UserBase` / `USER` 仍作为自定义段起点（若存在）
- [ ] 已 `rg` 确认无残留旧枚举引用
- [ ] 新子目录已写入 `middleware/CMakeLists.txt`
- [ ] 删除消息类型后，相关 `subscribe` / `publish` 已同步删除或迁移

---

## 6. memory_pool 使用说明

### 6.1 MemPool（固定块）

块大小档位：64 / 256 / 1024 / 4096 / 16384 字节，每档 16 块；超出最大档回退 `::operator new`。

```cpp
void* p = MemPool::instance().allocate(200);
// ...
MemPool::instance().deallocate(p);

// RAII
MemBlock block(MemPool::instance().allocate(64));
uint8_t* data = block.data();

// 运行时统计（供 debug dump 或监控）
auto stats = MemPool::instance().get_stats();
// stats[i]: block_size, total_blocks, used_blocks, free_blocks
```

### 6.2 BufferPool（引用计数，适合大 payload）

内部使用 `std::mutex` 保护 `buffers_` 与 `ref_count`，`allocate` / `acquire` / `release` / `get_ptr` 等接口可在多线程下直接调用。

```cpp
BufferId id = BufferPool::instance().allocate(1024);
uint8_t* ptr = BufferPool::instance().get_ptr(id);

// 跨模块传递时用 BufferRef
Message m;
m.payload = MsgPayload::from_buffer(id, 0, 1024);

BufferPool::instance().release(id);  // 或用 BufferHandle RAII

BufferHandle h(id);  // 析构自动 release

// 运行时统计
BufferPool::Stats st = BufferPool::instance().get_stats();
// st.capacity, st.used_count, st.bytes_used
```

**`MsgPayload::from_bytes` 行为**（API 不变）：

- `len ≤ 256`：Inline
- `len > 256`：自动 `BufferPool::allocate` 并转为 `BufferRef`（需已 `BufferPool::init()`）
- 池满时回退为 Inline 截断 256 字节

订阅者收到 `BufferRef` 后**必须**在处理完毕调用 `BufferPool::release(buf_id)`（或使用 `BufferHandle`）。

`init(max_buffers)` 会 `resize` 槽位数组；`BufferEntry` 使用普通 `uint32_t` 引用计数（非 `std::atomic`），以便满足 `std::vector` 元素可移动要求。

---

## 7. tread_pool 使用说明

非单例，由业务方持有生命周期：

```cpp
#include "thread_pool.hpp"

middleware::ThreadPool pool(4);  // 默认 THREAD_POOL_MAX_THREADS

pool.submit([]() {
    // 后台任务
});

auto fut = pool.submit_future([]() { return 42; });
int v = fut.get();

pool.submit_urgent([]() { /* 插队任务 */ });

pool.shutdown();  // 析构时也会调用
```

队列满时 `submit` / `submit_urgent` 返回 `false`。

`active_count()` 由池级 `std::atomic<size_t>` 统计正在执行的任务数；`Worker` 仅持有 `std::thread`（不含 per-thread `atomic`，避免 `std::vector<Worker>` 移动受限）。

---

## 8. 典型架构（Demo 前后端）

| 通道 | ModuleId | Service | UI 出口 |
|------|----------|---------|---------|
| Demo | `APP` | `DemoService` | `UiMsgProxy::send_demo_hello` / `register_demo_hello` |

```
PageHome 按钮
  → UiMsgProxy::send_demo_hello()
  → DemoHelloReq (UI → APP)
  → DemoService
  → DemoHelloRsp (APP → UI)
  → UiMsgProxy 回调 → lv_label_set_text
```

### 8.1 Demo Payload 约定

| 消息 | Payload |
|------|---------|
| `DemoHelloReq` | Empty（无 body） |
| `DemoHelloRsp` | Inline 字符串（如 `"HelloWorld"`） |

扩展新 Service 时可复用：`from_bytes` / `from_value` / `BufferRef`（见 §6.2）。

---

## 9. 线程安全与约束

| 组件 | 线程安全 |
|------|----------|
| `MsgBus::publish` / `deliver` | 订阅表 mutex；**handler 在锁外调用** |
| `MsgBus::publish_async` + `process` | 队列有 mutex；**process 只在单线程调用** |
| `MsgBus::pending_count` 等状态 API | 各自加锁；**禁止在已持同一把锁的路径内重入调用** |
| `MemPool` | 单例 + 内部 `mutex`，allocate/deallocate 可跨线程 |
| `BufferPool` | 单例 + 内部 `mutex`，allocate/acquire/release/get_ptr 可跨线程 |
| `RingBuffer` | 原子 head/tail/count，适合 SPSC；多写需外部同步 |
| `ThreadPool` | 任务队列有 mutex；`submit` 多线程安全；`shutdown` 会排空已入队任务 |
| `MW_LOG_*` / debug | 输出到 stderr；**必须在释放组件 mutex 之后调用** |

---

## 10. 已知限制（修改前请先读）

1. **`tread_pool` 目录名**：历史拼写错误，CMake 与 include 路径均使用 `tread_pool`，勿改成 `thread_pool` 除非全仓重命名。
2. **头文件路径**：使用 `#include "msg_bus.hpp"` 等，CMake 已注入 `message_bus/`、`memory_pool/`、`tread_pool/`、`debug/` 目录。
3. **与 LVGL**：middleware 不依赖 LVGL；UI 层负责在 `lv_timer` 或主循环中调用 `MsgBus::process()`（本仓库 `main` 中通常调用两次以同帧处理 Req+Rsp）。
4. **`BufferEntry` / `Worker` 与 `std::vector`**：成员中不宜放不可移动的 `std::atomic`；`BufferPool` 用 `mutex` + `uint32_t`，`ThreadPool` 用池级 `active_count_` 而非 per-worker 原子。
5. **扩展/裁剪模块与消息**：见 §5.5；核心只改 `msg_types.hpp`，已上线数值勿改号。
6. **`publish()` 同步路径**：仍可能在非 UI 线程直接触发 handler；跨线程更新 LVGL 务必用 `publish_async` + `process()`。
7. **`from_bytes` >256B**：返回 `BufferRef` 时，消费者必须 `release`；Inline 路径无需 release。
8. **调试日志**：持锁打日志或持锁查 `pending_count()` 会导致死锁；默认不开启 MEM/BUF 逐条日志是为保护 UI 帧率。

---

## 11. Agent 快速检查清单

- [ ] 根 CMake 已 `add_subdirectory(middleware)` 且 `target_link_libraries(... middleware::port)`
- [ ] 启动时 `MemPool` → `BufferPool` → `MsgBus` 依次 `init()`
- [ ] UI 线程定期 `MsgBus::instance().process()`
- [ ] 跨线程更新 UI 用 `publish_async`，不在后台线程直接操作 LVGL
- [ ] `from_bytes` / `BufferRef` 消息在消费后 `BufferPool::release`
- [ ] `publish_async` 失败时检查返回值（BufferRef 已由 MsgBus 自动 release）
- [ ] `BufferPool` / `MemPool` 多线程可直接调用
- [ ] 扩展/删除 `ModuleId`、`MsgType` 时只改 `msg_types.hpp` 并全仓搜索（见 §5.5）
- [ ] include 使用 `msg_bus.hpp` / `mem_pool.hpp` / `thread_pool.hpp` / `mw_debug.hpp`（由 CMake 注入路径）
- [ ] 调试前后端消息流：`-DMIDDLEWARE_DEBUG=ON`；发布版保持 OFF
- [ ] 需要内存池逐条日志时再加 `-DMIDDLEWARE_DEBUG_VERBOSE_POOL=ON`
- [ ] 新增 debug 日志时：持锁内禁止 `MW_LOG_*` 与 MsgBus 状态 API 重入

---

## 12. 最小示例（可复制）

```cpp
#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"

int main() {
    using namespace middleware;

    MemPool::instance().init();
    BufferPool::instance().init();
    MsgBus::instance().init();

    auto sub = MsgBus::instance().subscribe(
        MsgType::DemoHelloRsp, ModuleId::UI,
        [](const Message& msg) {
            if (msg.payload.type == MsgPayload::Type::Inline) {
                // msg.payload.data.bytes, msg.payload.length
            }
        });

    Message req(MsgType::DemoHelloReq, ModuleId::UI, ModuleId::APP);
    MsgBus::instance().publish_async(req);

    for (int i = 0; i < 100; ++i) {
        MsgBus::instance().process();
    }

    (void)sub;
    MsgBus::instance().deinit();
    BufferPool::instance().deinit();
    MemPool::instance().deinit();
    return 0;
}
```

---

## 13. 相关文档

| 文档 | 内容 |
|------|------|
| [ui/AGENTS.md](../ui/AGENTS.md) | PageHome、UiMsgProxy |
| [services/AGENTS.md](../services/AGENTS.md) | DemoService |
| `message_bus/msg_types.hpp` | 当前仅 `UI`/`APP`、`DemoHello*` |
| `debug/mw_debug.hpp` | `MW_LOG_*` 宏、`mw_debug_dump_*` API |
| `app/src/main.cpp` | Demo 最小集成 |

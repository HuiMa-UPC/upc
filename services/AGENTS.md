# services 模块 — Agent 使用指南

> 命名空间：`services`  
> CMake 目标：`services::port`  
> 依赖：[`middleware/AGENTS.md`](../middleware/AGENTS.md)  
> UI 协作：[`ui/AGENTS.md`](../ui/AGENTS.md)

---

## 1. 模块概览

| 文件 | 职责 | 状态 |
|------|------|------|
| `inc/service_base.hpp` / `src/service_base.cpp` | `ServiceBase` 生命周期、MsgBus 订阅/发布、可选独立工作线程 | 基类 |
| `inc/demo_service.hpp` / `src/demo_service.cpp` | Demo 前后端（`DemoHelloReq` → `HelloWorld`） | ✅ 独立线程运行 |

---

## 2. 目录结构

```
services/
├── CMakeLists.txt
├── AGENTS.md
├── inc/
│   ├── service_base.hpp
│   ├── demo_service.hpp
│   └── galvo_service.hpp
└── src/
    ├── service_base.cpp
    ├── demo_service.cpp
    └── galvo_service.cpp
```

```cpp
#include "service_base.hpp"
#include "demo_service.hpp"
```

---

## 3. CMake 集成

```cmake
add_subdirectory(services)
target_link_libraries(rongsu_ui PRIVATE services::port middleware::port)
```

当前编译 `service_base` + `demo_service` + `galvo_service`。

---

## 4. ServiceBase 生命周期

```
init()  → do_init()
start() → do_start()   // 在此 subscribe；若 use_worker_thread=true，同时启动工作线程
stop()  → do_stop()    // 若使用了工作线程，先 join 线程，再清空订阅
```

### 工具方法

| 方法 | 说明 |
|------|------|
| `subscribe(type, handler)` | 订阅消息，`handler` 中应调用 `on_message(msg)` |
| `publish(msg)` | **同步**发布，在当前线程立即执行 deliver（慎用） |
| `publish_async(msg)` | **异步**入队，由 `MsgBus::process()` 所在线程分发（推荐） |

---

## 5. 独立工作线程

`ServiceBase` 支持让每个 Service 在**独立的 `std::thread`** 中处理消息，避免耗时操作阻塞 LVGL/UI 主线程。

### 5.1 如何开启

在 `ServiceConfig` 中设置 `use_worker_thread = true`：

```cpp
DemoService::DemoService()
    : ServiceBase(ServiceConfig{
          "demo",
          static_cast<uint32_t>(middleware::ModuleId::APP),
          true   // ← 启用独立工作线程
      }) {}
```

### 5.2 线程模型

**未开启工作线程（默认）：**
```
UI 主线程
  └── MsgBus::process()
        └── deliver(msg)
              └── on_message(msg)
                    └── handle_message(msg)   // 同步执行，阻塞主线程
```

**开启工作线程：**
```
UI 主线程
  └── MsgBus::process()
        └── deliver(msg)
              └── on_message(msg)             // 仅把 msg push 到内部队列，立刻返回

DemoService 工作线程
  └── worker_loop()
        └── pop(msg)
              └── handle_message(msg)         // 在独立线程中处理
```

### 5.3 ⚠️ 线程安全红线

当 Service 运行在独立工作线程中时，`handle_message()` 的上下文**不是 UI 线程**。因此：

- **回发消息到 UI 时，必须用 `publish_async()`**，禁止用 `publish()`。
  - `publish()` 会在当前线程（工作线程）直接调用 deliver，导致 UI handler 在工作线程执行，破坏 LVGL 线程安全。
- **不要在工作线程中直接操作 LVGL 对象**（`lv_obj_t*` 等）。
- 如果需要进行耗时 IO（文件、网络），可以在 `handle_message()` 中执行；如需进一步并发，可再提交到 `middleware::ThreadPool`。

### 5.4 正确示例

```cpp
void DemoService::handle_message(const middleware::Message& msg) {
    if (msg.type != middleware::MsgType::DemoHelloReq) {
        return;
    }

    // 模拟耗时操作（如文件 IO、算法计算）
    // 此处位于独立工作线程，不会卡 UI
    auto result = do_heavy_work();

    middleware::Message rsp(middleware::MsgType::DemoHelloRsp,
        middleware::ModuleId::APP, middleware::ModuleId::UI);
    rsp.payload = middleware::MsgPayload::from_bytes(...);

    // ✅ 必须用 publish_async，让结果回到主线程分发
    publish_async(rsp);
}
```

---

## 6. DemoService

### 消息流

```
PageHome 按钮
  → UiMsgProxy::send_demo_hello()
  → DemoHelloReq (UI → APP)
  → MsgBus::deliver()
  → DemoService::on_message()   // 入队到工作线程队列（立刻返回）
  → DemoService::handle_message // 在工作线程执行
  → DemoHelloRsp (APP → UI, payload="HelloWorld")
  → UiMsgProxy::register_demo_hello
```

### 应用入口

```cpp
services::DemoService demo_service;
demo_service.init();
demo_service.start();
```

---

## 7. 添加新 Service

1. 在 `middleware/message_bus/msg_types.hpp` 增加 `ModuleId` / `MsgType`。
2. 新建头文件 `inc/xxx_service.hpp`、源文件 `src/xxx_service.cpp`，继承 `ServiceBase`。
3. 在 `app/src/main.cpp` 中实例化、init()、start()。
4. 在 `UiMsgProxy` 封装 send/register，页面只调 `UiMsgProxy`。

---

## 8. Agent 快速检查清单

- [ ] `do_start()` 内 `subscribe`，回复 UI 用 `publish_async` + `target_id = ModuleId::UI`
- [ ] UI 不直接碰 MsgBus
- [ ] 若 Service 开启了 `use_worker_thread = true`，`handle_message()` 中**禁止**调用 `publish()`（同步分发）
- [ ] `stop()` 会自动 join 工作线程并清空内部消息队列，无需子类额外处理

---

## 9. 相关文档

| 文档 | 内容 |
|------|------|
| [middleware/AGENTS.md](../middleware/AGENTS.md) | MsgBus、MsgType、ThreadPool |
| [ui/AGENTS.md](../ui/AGENTS.md) | PageHome、UiMsgProxy |
| `app/src/main.cpp` | Demo 集成示例 |

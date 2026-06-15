# common/middleware — 进程内消息总线

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md) · CMake：`middleware::port`  
> 命名空间：`middleware`

## 子模块

| 目录 | 职责 |
|------|------|
| `message_bus/` | `MsgBus`、`MsgType`、`MsgPayload`、异步队列 |
| `memory_pool/` | 固定块池、`BufferPool` |
| `thread_pool/` | 工作线程池（目录名历史拼写 `tread_pool`） |
| `debug/` | `MW_LOG_*`（middleware 内部调试） |

无 LVGL 依赖；UI / Glavo / test 均可链接。

## 核心类型（源码为准）

- **`Message`**：`type`、`source_id`、`target_id`、`payload`
- **`ModuleId`**：`UI`、`APP_GLAVO`、`APP_CAMERA` 等
- **`MsgType`**：见 [contract/README.md](../contract/README.md) 总表
- **`MsgPayload`**：Inline / BufferRef

改消息：**contract 四步** + `msg_types.hpp`，不要只改文档。

## 用法要点

```cpp
// 订阅（Subscription 须持久保存）
auto sub = MsgBus::instance().subscribe(type, module_id, handler);

// 异步投递（跨线程 / 回复 UI 推荐）
MsgPort::instance().publish_async(msg);

// UI / 后端主循环消费队列
MsgPort::instance().process(timeout_ms);
```

跨进程时 UI/Glavo 用 **`MsgPort`**，内部再进 `MsgBus`。详见 [ipc/IPC.md](../ipc/IPC.md)。

## 打印数据路径（不经 MsgBus  payload）

```
ParserManager → RingBuffer → McuLink（TCP + 串口）
```

## 相关文档

- [contract/README.md](../contract/README.md) — 消息命名与 Payload
- [glavo/GLAVO.md](../../glavo/GLAVO.md) · [ui/UI.md](../../ui/UI.md)

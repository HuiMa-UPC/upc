# upc::parser — 切片格式解析库

独立静态库，不启动进程。输出为 `RingBuffer` 中的 MCU 运动包（`rb_packet_t`）。

## CMake 目标

| 目标 | 别名 | 说明 |
|------|------|------|
| `upc_parser` | `upc::parser` | 框架 + CalParser + SliceRingBuffer + Manager |
| `upc_parser_gcode` | `upc::parser_gcode` | G-code 插件（需 `--whole-archive`） |
| `upc_parser_ovf` | `upc::parser_ovf` | OVF 插件 + protobuf |

## 链接示例

```cmake
target_link_libraries(my_proc PRIVATE
    upc::debug
    -Wl,--whole-archive upc::parser_gcode upc::parser -Wl,--no-whole-archive
)
```

## 用法概要

```cpp
#include "parser_config.hpp"
#include "parser_context.hpp"
#include "parser_manager.hpp"

upc::parser::ParserContext::instance().load_calibration(CAL_DEFAULT_FILE_PATH);

upc::parser::ParserManager::instance().set_ring_buffer_sink(
    [](upc::parser::RingBuffer* rb) { /* 绑定下发层 */ });

upc::parser::ParserManager::instance().open_file("/path/file.gcode");
upc::parser::ParserManager::instance().start_parse();
```

Glavo 进程通过 `upc::glavo::glavo_parser_bind_init()` 将 sink 绑到 `TcpSender`。

## 命名空间

- `upc::parser::` — 解析相关
- `upc::glavo::` — 打印下发与 MsgBus 编排（`upc_glavo` 库）

# parser/ — 切片解析库

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 命名空间：**`upc::parser::`** · 独立静态库，**不启动进程**

## 职责

将 `.gcode` / `.ovf` 等切片文件解析为 **MCU 运动包**，写入 `RingBuffer`（`rb_packet_t`），由下游（如 `McuLink`）消费下发。

```
切片文件 → IParserWorker（解析线程）→ RingBuffer → 消费者（Glavo: McuLink）
```

## 目录

| 路径 | 说明 |
|------|------|
| `lib/` | 框架核心：`parser_manager`、`parser_registry`、`parser_context`、`cal_parser`、`slice_ring_buffer`、`parser_output`、`parser_worker`、`parser_config` |
| `gcode/` | G-code 插件 → `upc::parser_gcode` |
| `ovf/` | OVF + protobuf → `upc::parser_ovf` |

## CMake 目标

| 目标 | 别名 | 说明 |
|------|------|------|
| `upc_parser` | `upc::parser` | 框架 + CalParser + RingBuffer + Manager |
| `upc_parser_gcode` | `upc::parser_gcode` | G-code 插件（需 `--whole-archive` 注册） |
| `upc_parser_ovf` | `upc::parser_ovf` | OVF 插件 + protobuf |

```cmake
target_link_libraries(my_proc PRIVATE
    upc::debug
    -Wl,--whole-archive upc::parser_gcode upc::parser -Wl,--no-whole-archive
)
```

## 输出模型

| 包类型 | 含义 |
|--------|------|
| `RB_PKT_MOTION` | 11 字节运动子包（X/Y/速度/E 等，已校准） |
| `RB_PKT_LAYER_MARK` | 换层标记；`data[0..1]` 为层厚 μm（LE） |

进度 API：`get_total_lines()`、`get_parsed_lines()`、`get_total_motion_packets()`。

## 校准文件 cal.csv

| 行 | 格式 | 含义 |
|----|------|------|
| 2 | `CAL,scale,range,speed` | 图像/速度增益 |
| 3 | `Table,nx,ny,grid_x,grid_y` | 畸变网格尺寸 |
| 4 | `X,<offset>,Y,<offset>` | 中心平移（mm，可正可负） |
| 5+ | `X1`…`Xn` / `Y1`…`Yn` | 双线性畸变表 |

启动时 `ParserContext::load_calibration()` 加载一次；各 Worker 在 `apply_calibration_from_context()` 中复制 `CalData`。

**新 Worker 组包时**应调用 `cal_apply_xy()` 将切片坐标转为校准后物理坐标（中心平移 + 网格 Δ），再乘 `scale_gain` 并编码 MCU 包，与 G-code/OVF 保持一致：

```cpp
float out_x = 0.f, out_y = 0.f;
cal_apply_xy(&cal_data_, slice_x, slice_y, &out_x, &out_y);
const float ox = out_x * image_scale_gain_;
```

## 基本用法

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

## Glavo 集成

- **禁止**在 `glavo/` 里 `#include` 具体 `GcodeWorker` / `OvfWorker`
- 通过 `ParserManager::active()` 访问当前 worker
- 启动时 `upc::glavo::glavo_parser_bind_init()` 将 RingBuffer 绑到 `McuLink`

详见 [glavo/GLAVO.md](../glavo/GLAVO.md)。

## 红线

- **UI / test 禁止**链接 `parser/` 业务库
- 新格式：实现 `IParserWorker` + `ParserRegistry::register_parser`，独立 CMake 插件目标
- 插件静态注册需 **`--whole-archive`**，否则链接器可能剥离 `*_register.cpp`
- OVF 依赖 **protobuf**；无 protobuf 环境可只链 `upc_parser_gcode`

## 新增切片格式（概要）

1. `parser/<fmt>/` — `*Worker` 实现 `IParserWorker`，`*_register.cpp` 调 `register_parser`
2. `parser/CMakeLists.txt` — 新静态库 + `upc::parser_<fmt>` alias
3. 消费方 CMake — `--whole-archive` 链入新插件
4. `GlavoService` 目录过滤 — `ParserRegistry::all_extensions()`

## 相关文档

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)

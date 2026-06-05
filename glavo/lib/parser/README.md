# 切片格式解析 — 协议开发者指南

本目录用于放置**各切片文件格式的解析模块**。每个子目录对应一种协议，编译进固件后自动注册；删除子目录则该格式不再可用，主工程仍可正常编译运行。

当前已有实现：`gcode/`（`.gcode`、`.nc`）。

---

## 1. 架构位置

```
GlavoService（编排）
    ↓
ParserManager（同一时刻只有一个活跃解析器）
    ↓
IParserWorker  ← 你的实现
    ↓
RingBuffer（统一输出）
    ↓
TcpSender → MCU
```

你只需实现 **`IParserWorker` + 注册文件**，不需要修改 UI、GlavoService、TcpSender。

---

## 2. 快速开始：添加新格式

假设新格式 ID 为 `myfmt`，后缀为 `.mf`：

### 2.1 创建目录

```
glavo/lib/parser/myfmt/
├── myfmt_worker.hpp
├── myfmt_worker.cpp
└── myfmt_register.cpp
```

### 2.2 实现接口

继承 `IParserWorker`（定义于 `parser/parser_worker.hpp`）：

```cpp
#include "parser_worker.hpp"

namespace services {

class MyfmtWorker : public IParserWorker {
public:
    static MyfmtWorker& instance();

    int  open_file(const std::string& filepath) override;
    void start_parse() override;
    void stop_parse() override;
    void close_file() override;

    int  get_total_lines() const override;
    int  get_parsed_lines() const override;
    int  get_total_motion_packets() const override;
    bool is_parsing() const override;
    bool is_file_read_exhausted() const override;
    bool is_file_open() const override;

    RingBuffer& ring_buffer() override;
    uint16_t get_layer_height_um() const override;

    // ...
};

} // namespace services
```

### 2.3 注册后缀

`myfmt_register.cpp`：

```cpp
#include "parser_registry.hpp"
#include "myfmt_worker.hpp"

namespace {

struct MyfmtRegistrar {
    MyfmtRegistrar()
    {
        services::ParserRegistry::instance().register_parser({
            .id = "myfmt",
            .extensions = {".mf"},
            .factory = []() -> services::IParserWorker* {
                return &services::MyfmtWorker::instance();
            },
        });
    }
};

[[gnu::used]] static MyfmtRegistrar myfmt_parser_registrar;

} // namespace
```

### 2.4 编译

源文件放在 `parser/<协议名>/` 下会被 CMake 自动收录进 **`services_parsers`** 静态库。

根目录 `CMakeLists.txt` 已用 `--whole-archive` 链接该库，**无需手动改链接脚本**。若在新工程里集成，必须保证 parser 插件库整库链接，否则 `*_register.cpp` 里的静态注册会被链接器裁掉，表现为**目录里看不到任何已注册后缀的文件**。

各 `parser/*/` 协议子目录会自动加入 include 路径；新增插件时需在 `glavo/lib/CMakeLists.txt` 的 `SERVICES_PARSER_SOURCES` 中登记源文件。

### 2.5 验证

- 重启后，文件列表中应出现 `.mf` 文件
- 选中文件可预览、可打印
- 删除 `parser/myfmt/` 整个目录后重编译，`.mf` 不再出现在列表中

---

## 3. 必须实现的接口语义

| 方法 | 要求 |
|------|------|
| `open_file` | 打开文件并做预扫描（统计行数/包数等）；返回 `0` 成功，`-1` 失败 |
| `start_parse` | 启动**独立线程**解析，结果写入内部 `RingBuffer` |
| `stop_parse` | 请求停止、**join 线程**、清空 RingBuffer |
| `close_file` | `stop_parse` + 关闭文件句柄 |
| `get_total_lines` | 预扫描总量；G-code 为行数，其他格式可映射为可报告整数 |
| `get_parsed_lines` | 当前已处理量（解析过程中递增） |
| `get_total_motion_packets` | 已产生的 `RB_PKT_MOTION` 数量（解析过程中递增） |
| `is_parsing` | 解析线程是否运行中 |
| `is_file_read_exhausted` | 文件内容是否已全部读取 |
| `is_file_open` | 文件是否已 open |
| `ring_buffer` | 返回本 worker 持有的 `RingBuffer` 引用（生命周期与 worker 一致） |
| `get_layer_height_um` | 当前层厚（μm），供串口换层握手；无层概念时可返回默认值 |

### 线程模型

- 解析在 **worker 自有线程** 中运行
- 通过 `RingBuffer::put_wait(pkt, stop_req_)` 入队；队列满时阻塞，避免忙等
- 队列不满时适当 `sleep`（参考 `gcode_worker.cpp` 的 chunk + yield）
- `stop_parse()` 必须能唤醒阻塞中的 `put_wait`

### 单例建议

参考 `GcodeWorker::instance()` 使用 function-local static 单例。同一时刻 `ParserManager` 只激活一个 worker，但单例便于 RingBuffer 地址在 `TcpSender` 初始化阶段保持稳定。

---

## 4. 统一输出格式（RingBuffer）

所有协议**必须**输出相同结构的 `rb_packet_t`（定义于 `services/glavo/ring_buffer.hpp`），下游 TcpSender 只认这一种格式。

### 4.1 包类型

| type | 值 | 含义 |
|------|----|------|
| `RB_PKT_MOTION` | 0 | 一条运动指令 |
| `RB_PKT_LAYER_MARK` | 1 | 层切换标记（换层握手） |

### 4.2 运动包 `RB_PKT_MOTION`（11 字节 data）

| 偏移 | 字段 | 说明 |
|------|------|------|
| `data[0]` | 固定 `0x01` | 子包类型标识 |
| `data[1..2]` | X | uint16 **小端**，坐标映射值（见 4.4） |
| `data[3..4]` | Y | uint16 **小端**，坐标映射值 |
| `data[5..6]` | Z | TCP 运动包中**固定为 0**（层厚走 LAYER_MARK） |
| `data[7..8]` | 速度 | uint16 **小端**，`speed * speed_gain` 取整 |
| `data[9..10]` | E | uint16 **小端**，挤出：`>0 → 1`，否则 `0` |

入队示例（与 G-code 一致）：

```cpp
rb_packet_t pkt{};
pkt.type = RB_PKT_MOTION;
pkt.data[0] = 0x01;
// data[1..2] = X, data[3..4] = Y, data[5..6] = 0
// data[7..8] = speed, data[9..10] = e
rb_.put_wait(pkt, stop_req_);
```

### 4.3 层标记 `RB_PKT_LAYER_MARK`

```cpp
rb_packet_t pkt{};
pkt.type = RB_PKT_LAYER_MARK;
std::memset(pkt.data, 0, sizeof(pkt.data));
rb_layer_mark_set_height_um(pkt, layer_height_um);  // data[0..1] = uint16 LE (μm)
rb_.put_wait(pkt, stop_req_);
```

每开始新层前插入一条 LAYER_MARK。G-code 对应 `M3000` 指令。

### 4.4 坐标映射算法

物理坐标 → uint16 映射（与 G-code 保持一致）：

```cpp
// 1. 可选：振镜校准补偿（见第 5 节）
// 2. 缩放：ox = x * scale_gain, oy = y * scale_gain
// 3. 限幅到 [-range_gain, +range_gain]
// 4. 归一化：normalized = coord / range_gain
// 5. 映射：offset = (int)(normalized * 32767.0f + 32768.0f)
// 6. 拆成 uint16 小端写入 data[1..2] / data[3..4]
```

完整参考实现见 `parser/gcode/gcode_worker.cpp` 中的 `coordinate_to_offset_bytes()` 与 `build_and_push_packet()`。

---

## 5. 校准数据（ParserContext）

校准在 **`GlavoService::do_init()`** 阶段统一加载，解析器**按需读取**，不重复读文件。

```cpp
#include "parser_context.hpp"

void MyfmtWorker::apply_calibration()
{
    const ParserContext& ctx = ParserContext::instance();

    if(ctx.cal_valid()) {
        // 使用 ctx.cal_data() 做 XY 网格补偿
        // ctx.scale_gain(), ctx.range_gain(), ctx.speed_gain()
    } else {
        // 使用 ctx 中的默认增益（加载失败时已填默认值）
    }
}
```

### CalData 字段（`cal_parser.hpp`）

| 字段 | 说明 |
|------|------|
| `valid` | 校准网格是否有效 |
| `nx`, `ny` | 网格尺寸 |
| `x_min/max`, `y_min/max` | 网格范围 |
| `z_dx`, `z_dy` | row-major 补偿量 |
| `has_gains` | CSV 是否含增益行 |
| `scale_gain`, `range_gain`, `speed_gain` | 图像缩放 / 范围 / 速度增益 |

### 补偿接口

```cpp
double dx = 0, dy = 0;
if(cal_interpolate_delta(&cal_data, x, y, &dx, &dy)) {
    out_x += (float)dx;
    out_y += (float)dy;
}
```

默认校准文件路径：`/home/CAL/cal.csv`（`CAL_DEFAULT_FILE_PATH`）。

**不需要校准的格式**（如坐标已是设备空间）可完全忽略 `ParserContext`。

建议在 `open_file()` 时拷贝一份到 worker 成员，避免解析线程与初始化线程竞态（参考 `GcodeWorker::apply_calibration_from_context()`）。

---

## 6. 后缀与文件列表

- 在 `register_parser` 中声明的 `extensions` 决定文件列表过滤
- 后缀**统一小写**存储与比较（`Part.GCODE` 与 `part.gcode` 等效）
- 扩展名需带点，如 `{".gcode", ".nc"}`
- 同一后缀注册多个解析器时的冲突策略**暂未实现**，请勿重复注册

---

## 7. 不要做的事

| 禁止 | 原因 |
|------|------|
| 在 GlavoService / TcpSender 中 `#include` 你的 worker 头文件 | 破坏编译期插拔 |
| 输出非 `rb_packet_t` 格式 | TcpSender 无法消费 |
| 在解析线程中操作 LVGL / 直接 publish 给 UI | 线程安全 |
| 在 worker 内重复 `load_calibration` 读 CSV | 应使用 `ParserContext` |
| 阻塞解析线程且不响应 `stop_req_` | 无法终止打印 |

---

## 8. 参考文件

| 文件 | 内容 |
|------|------|
| `parser/gcode/gcode_worker.cpp` | 完整解析 + 入队 + 校准 + 线程模型 |
| `parser/gcode/gcode_register.cpp` | 注册模板 |
| `parser/parser_worker.hpp` | 接口定义 |
| `parser/parser_context.hpp` | 校准读取 |
| `glavo/ring_buffer.hpp` | 输出包结构 |
| `glavo/cal_parser.hpp` | 校准数据结构与插值 API |

---

## 9. 调试

```bash
cmake -DSERVICES_DEBUG=ON ...
```

解析进度、坐标映射等日志会在 `SERVICES_DEBUG` 开启时输出。TcpSender 链路日志用 `-DTCP_DEBUG=ON`。

---

## 10. 常见问题

**Q：删除我的协议目录后工程编译失败？**  
A：检查是否有其他文件 `#include` 你的头文件。只有 `*_register.cpp` 应引用 worker 头文件。

**Q：文件在磁盘上但 UI 看不到？**  
A：1）确认后缀已在 `register_parser` 中注册；2）确认已重新编译且链接了 `services_parsers`（根 `CMakeLists.txt` 的 `--whole-archive`）；3）用 `-DSERVICES_DEBUG=ON` 编译，若日志出现 `no parser registered` 说明静态注册未生效。

**Q：打印进度不准？**  
A：确保 `get_total_lines`、`get_parsed_lines`、`get_total_motion_packets` 在解析过程中正确更新。

**Q：二进制格式没有「行数」？**  
A：当前 `FileInfoPayload.total_lines` 仍用于 UI 预览；可暂用层数或预计 motion 包数填充，后续 MsgBus 会扩展更通用的进度字段。

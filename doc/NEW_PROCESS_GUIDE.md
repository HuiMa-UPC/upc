# 新增进程与日志模块指南

> 适用：在 UPC 多进程架构下新增业务进程，或为新进程/子模块增加结构化日志。  
> 索引：[ARCHITECTURE.md](ARCHITECTURE.md) · [LOG_GUIDE.md](LOG_GUIDE.md) · [common/COMMON.md](../common/COMMON.md)

---

## 1. 架构前提

当前仓库支持两种协作方式，**契约与日志规则相同**：

| 方式 | 目录示例 | 说明 |
|------|----------|------|
| **master 单体编译** | `master/app/<proc>/` | `./build.sh` 一次编全部进程 |
| **独立工程** | `ui/`、`glavo/`、`camera/` | 各目录自有 `CMakeLists.txt` + `build.sh`，通过 `3rdparty/commom/` 链接预编译 SDK |

无论哪种方式：

- 跨进程 ABI **只在** `master/common/upc_contract.hpp` 维护一份源码
- 各独立工程的 `3rdparty/commom/include/upc_contract.hpp` 为**同步副本**，禁止在副本上单独修改
- 业务进程均为 Broker **Client**；控制消息走 `MsgPort`，大流量像素走 dma fd + SHM 元数据环（见 COMMON.md）

```
rongsu_broker ──listen──► /tmp/upc_broker.sock ◄── 各业务进程 (Client)
```

---

## 2. 新增进程：总览 checklist

按顺序完成，打勾后再提测：

- [ ] **契约** — `upc_contract.hpp` 增加 `ModuleId`、`process_config_*()`、（如需）`MsgType` + Payload
- [ ] **文档** — `common/COMMON.md` 消息总表 / 进程表；`doc/ARCHITECTURE.md` 进程行
- [ ] **common 实现** — 若改动 `ipc/`、`middleware/` 等 `.cpp`，重编 `libupc_common.a` 并同步到各独立工程
- [ ] **进程代码** — `main.cpp`（`start|stop|status|run`）+ 业务 `lib/`
- [ ] **构建** — `CMakeLists.txt` + `build.sh`（独立工程）或 `master/app/CMakeLists.txt`（单体）
- [ ] **启动脚本** — `scripts/rongsu.sh` 启动顺序（Broker 最先）
- [ ] **日志** — 见 §4（新进程至少调用 `log_set_process_name`；有新子模块则加 `UPC_MOD_*`）
- [ ] **联调** — Broker 已起、对端 `IPC_RX` / `IPC_TX` 日志可对照

---

## 3. 新增进程：分步说明

### 3.1 第一步 — 改契约 `upc_contract.hpp`

**唯一编辑入口**：`master/common/upc_contract.hpp`（改完后同步到各独立工程 `3rdparty/commom/`）。

#### 3.1.1 增加 `ModuleId`

在 `middleware::ModuleId` **末尾追加**（禁止改已有枚举值）：

```cpp
enum class ModuleId : uint32_t {
    UI         = 0x01,
    APP_GLAVO  = 0x02,
    APP_CAMERA = 0x03,
    APP_FOO    = 0x04,  /**< 新进程 rongsu_foo 的身份 */
};
```

命名建议：`APP_<业务名>`，与可执行文件职责一致。

#### 3.1.2 增加进程注册表 §3

在 `process_config_*()` 旁增加：

```cpp
inline ProcessConfig process_config_foo() {
    return ProcessConfig{
        "/var/run/rongsu_foo.pid",
        "/var/log/rongsu_foo",   // 实际文件 rongsu_foo1.log ~ rongsu_foo3.log
        "rongsu_foo",
    };
}
```

| 字段 | 约定 |
|------|------|
| `pid_file` | `/var/run/rongsu_<name>.pid` |
| `log_base` | `/var/log/rongsu_<name>`（三文件 5MB 轮转，见 `log_rotating.cpp`） |
| `name` | 与可执行文件名一致，用于日志 `proc=` 字段 |

#### 3.1.3 增加消息（若需与其它进程通信）

遵循 COMMON.md **「新增消息固定四步」**：

1. **`upc_contract.hpp`** — 在对应 `MsgType` 分段**末尾**追加枚举 + POD Payload + 注释  
   - 禁止在分段中间插入（会破坏已有枚举数值）  
   - 新分段使用新基址（如 `0x1060`），与相邻段留间隔  
2. **发送侧进程** — `subscribe` + `handle_*` 或 `publish_*`  
3. **UI 侧**（若需界面）— `XxxMsgProxy` 的 `send_*` + `register_*`  
4. **Broker** — 一般无需改代码；`target_id` 指向新 `ModuleId` 时自动转发  

消息命名：`<域>_<动作>_Req|Rsp|Ntf|Done`（如 `FOO_Status_Req`）。

同步更新 [`common/COMMON.md`](../common/COMMON.md) 消息总表一行。

---

### 3.2 第二步 — 编写 `main.cpp`

参照 `app/camera/main.cpp` / `app/glavo/main.cpp` 模板：

```cpp
#include "process_ctl.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"
#include "upc_sys_monitor.hpp"
#include "rs_log.hpp"
// #include "foo_service.hpp"

namespace {

const ProcessConfig kProc = process_config_foo();

const char* broker_socket_path() {
    const char* env = std::getenv("UPC_BROKER_SOCKET");
    return (env && env[0]) ? env : UPC_BROKER_SOCKET_DEFAULT;
}

int start_foo(char daemon) {
    upc::log_set_process_name(kProc.name);  // 必须：日志 proc= 字段
    process_install_signals();

    if (daemon) {
        // process_check_running → process_daemonize → process_write_pid
    }

    auto& port = middleware::MsgPort::instance();
    if (!port.set_mode_ipc_client(middleware::ModuleId::APP_FOO,
                                  broker_socket_path())) {
        UPC_LOG_FOO_SERVICE(Error, "IPC connect failed on %s", broker_socket_path());
        return 4;
    }
    if (!port.init()) {
        UPC_LOG_FOO_SERVICE(Error, "MsgPort init failed");
        return 5;
    }

    // 业务 init / start ...

    while (!g_shutdown_requested) {
        port.process(50);          // 泵 IPC 队列
        upc_sys_monitor_tick();    // 可选：HostMonitor
    }

    port.shutdown();
    if (daemon) process_remove_pid(kProc.pid_file);
    return 0;
}

} // namespace

// main：解析 start|stop|status|run，与现有进程一致
```

要点：

- `set_mode_ipc_client(本进程 ModuleId, broker_socket_path())`
- 主循环必须定期 `port.process()`
- `start` 模式走 `process_daemonize(kProc.log_base)`，日志写入 `/var/log/rongsu_foo{1,2,3}.log`

---

### 3.3 第三步 — CMake 构建

#### 方式 A：独立工程（推荐，与 `ui/` / `glavo/` / `camera/` 一致）

在仓库根新建 `foo/`：

```
foo/
  CMakeLists.txt          # project + 内联 3rdparty/commom
  build.sh                # 产物 → foo/output/rongsu_foo
  cmake/k1mini-buildroot-toolchain.cmake
  main.cpp
  lib/                    # 业务源码
  3rdparty/commom/        # libupc_common.a + include/（从 master 同步）
```

`CMakeLists.txt` 骨架（无 LVGL / parser / protocol 时，参考 `camera/CMakeLists.txt`）：

```cmake
cmake_minimum_required(VERSION 3.16)
# project(rongsu_foo) + toolchain + UPC_DEBUG
# ... 内联 3rdparty/commom 预编译库（与 ui/camera 相同块）...

add_executable(rongsu_foo
    main.cpp
    lib/foo_service.cpp
    # ...
)

target_include_directories(rongsu_foo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)

target_link_libraries(rongsu_foo PRIVATE upc::ipc upc::process upc::debug)
```

`build.sh`：复制 `camera/build.sh`，将 `rongsu_camera` 替换为 `rongsu_foo`。

#### 方式 B：master 单体工程

1. 新建 `master/app/foo/`，编写 `CMakeLists.txt`（参考 `app/camera/CMakeLists.txt`）
2. `master/app/CMakeLists.txt` 增加 `add_subdirectory(foo)`
3. `master/build.sh` 的 `artifacts` 数组增加 `app/foo/rongsu_foo`

#### 链接边界（红线）

| 进程类型 | 允许链接 | 禁止 |
|----------|----------|------|
| 普通后端 | `upc::common`（或别名） | `lvgl::port`、`ui::port` |
| UI | `upc::common` + `ui::port` + `lvgl::port` | `app/glavo/lib`、`app/parser` |
| Glavo | `upc::common` + `upc::protocol` + `upc::parser` | `lvgl` |

---

### 3.4 第四步 — 同步 common SDK

契约或 common 实现变更后：

```bash
# 1. 在 master 重编 common
cd master && ./build.sh

# 2. 同步到各独立工程（示例）
for d in ui glavo camera foo; do
  install -D -m644 master/output/install/lib/libupc_common.a \
    "$d/3rdparty/commom/lib/libupc_common.a"
  rsync -a --delete master/output/install/include/common/ \
    "$d/3rdparty/commom/include/"
done
```

仅改 `upc_contract.hpp` 且未动 common `.cpp` 时，可只同步头文件目录。

---

### 3.5 第五步 — 启动脚本与架构索引

1. **`scripts/rongsu.sh`** — 在 `start_daemon` 中按依赖顺序启动（Broker → 后端 → UI）：

   ```bash
   "$BROKER" start
   "$GLAVO" start
   "$CAMERA" start
   "$FOO" start      # 若 UI 依赖它，放在 UI 之前
   "$UI" start
   ```

2. **`doc/ARCHITECTURE.md`** — 进程表增加一行（可执行文件、入口、`*.md`、日志路径）

3. **`app/foo/FOO.md`**（或独立工程内 `FOO.md`）— 职责、IPC 订阅表、日志 module 说明

---

### 3.6 第六步 — 验证

```bash
# 独立工程
cd foo && ./build.sh

# 或 master 全量
cd master && ./build.sh

# 联调
master/scripts/rongsu.sh start
tail -f /var/log/rongsu_foo*.log
grep 'module=IPC' /var/log/rongsu_foo*.log    # 应看到 connect / peer
grep 'proc=rongsu_foo' /var/log/rongsu_foo*.log
```

跨进程消息：发送方 `IPC_TX`、接收方 `IPC_RX` + `IPC_DELIVER` 应对照出现（见 [LOG_GUIDE.md](LOG_GUIDE.md) §4）。

---

## 4. 新增日志模块

新进程通常**至少**在入口调用 `upc::log_set_process_name(kProc.name)`，即可在每条 `UPC_LOG_*` 行获得 `proc=rongsu_foo`。

若业务代码需要**独立子模块开关**（如 `module=FooService`），需改三处。

### 4.1 在 `upc_contract.hpp` §2 增加子开关

`UPC_DEBUG=ON` 分支：

```cpp
#ifndef UPC_MOD_FOO_SERVICE
#define UPC_MOD_FOO_SERVICE 1
#endif
```

`UPC_DEBUG=OFF` 分支（`#else` 内）同步增加：

```cpp
#define UPC_MOD_FOO_SERVICE 0
```

约定：

- 宏名：`UPC_MOD_<大写下划线>`，与业务模块对应
- 默认值：`1` 开启 / `0` 关闭（高噪声模块如 `UPC_MOD_CAMERA_PREVIEW` 可默认 `0`）
- **两处都要改**（ON 与 OFF 分支），否则关闭总开关时行为不一致

### 4.2 在 `rs_log.hpp` 增加打印宏

```cpp
#define UPC_LOG_FOO_SERVICE(lvl, fmt, ...) \
    UPC_LOG_MOD_(UPC_MOD_FOO_SERVICE, lvl, "SVC", "FooService", fmt, ##__VA_ARGS__)
```

| 参数 | 含义 | 常用值 |
|------|------|--------|
| `lvl` | `Error` / `Warn` / `Info` / `Debug` / `Trace` | 按场景 |
| `src` | 日志分层 | `MW` 中间件 / `SVC` 服务 / `SYS` 系统 / `PARSER` 解析 / `UI` 界面 |
| `module` | `grep module=` 用的标签 | 如 `FooService`、`McuLink` |

实现位于 `common/debug/rs_log.cpp`，**一般无需修改**。

### 4.3 业务代码中使用

```cpp
#include "rs_log.hpp"

UPC_LOG_FOO_SERVICE(Info, "ready IPC broker %s", broker_socket_path());
UPC_LOG_FOO_SERVICE(Debug, "handled request id=%u", req_id);
UPC_LOG_FOO_SERVICE(Error, "init failed: %s", reason);
```

单行格式（见 LOG_GUIDE.md §2）：

```
2017-08-05 12:56:07.610562 level=INFO proc=rongsu_foo src=SVC module=FooService ready IPC broker /tmp/upc_broker.sock
```

过滤：

```bash
grep 'module=FooService' /var/log/rongsu_foo*.log
grep 'proc=rongsu_foo'   /var/log/rongsu_foo*.log
```

### 4.4 文档与编译开关

| 文件 | 动作 |
|------|------|
| [LOG_GUIDE.md](LOG_GUIDE.md) §3 | 模块标签速查表增加一行 |
| [common/COMMON.md](../common/COMMON.md) Debug 节 | `UPC_MOD_*` 表增加一行 |
| 进程 `*.md` | 关键日志关键字表（可选） |

编译时关闭某一模块日志：

```bash
cmake -B build -DUPC_DEBUG=ON \
  -DCMAKE_CXX_FLAGS="-DUPC_MOD_FOO_SERVICE=0"
```

关闭全部结构化日志：

```bash
cmake -B build -DUPC_DEBUG=OFF
```

---

## 5. 仅新增日志、不新增进程

若只在**已有进程**内增加子模块日志（不新加 `ModuleId`）：

1. 完成 §4.1 ~ §4.4（`UPC_MOD_*` + `UPC_LOG_*` + 文档）
2. 在业务 `.cpp` 中调用新宏
3. 同步 `rs_log.hpp` 到各独立工程 `3rdparty/commom/include/debug/`（若 common 未重编，仅同步头文件即可）
4. 重编该进程：`./build.sh`

**不需要**改 `ModuleId`、`scripts/rongsu.sh` 或 Broker。

---

## 6. 文件改动速查表

### 新增进程

| 文件 | 必改 | 说明 |
|------|------|------|
| `common/upc_contract.hpp` | ✓ | `ModuleId`、§3 `process_config_*`、可选 `MsgType`/Payload |
| `common/COMMON.md` | ✓ | 消息总表、进程说明 |
| `doc/ARCHITECTURE.md` | ✓ | 进程索引行 |
| `app/<proc>/` 或独立 `<proc>/` | ✓ | 源码 + CMake |
| `scripts/rongsu.sh` | ✓ | 启动/停止顺序 |
| `master/build.sh` | 单体时 | `artifacts` 列表 |
| `common/*.cpp` | 按需 | 改实现则需重编 `libupc_common.a` |
| `common/debug/rs_log.hpp` | 按需 | 有新子模块日志时 |

### 仅新增日志模块

| 文件 | 必改 |
|------|------|
| `common/upc_contract.hpp` §2 | ✓ |
| `common/debug/rs_log.hpp` | ✓ |
| `doc/LOG_GUIDE.md` | 建议 |
| `common/COMMON.md` Debug 节 | 建议 |

---

## 7. 命名与评审

- **契约变更**：`upc_contract.hpp` + COMMON.md 消息总表，需 **UI 组 + 相关后端组** 评审（见 COMMON.md 文首）
- **ModuleId / MsgType**：一经发布勿改数值；废弃消息保留枚举项，注释 `@deprecated`
- **日志 module 名**：与 `UPC_LOG_*` 宏中字符串一致，便于 `grep module=`

---

## 8. 相关文档

| 文档 | 内容 |
|------|------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 进程与目录索引 |
| [LOG_GUIDE.md](LOG_GUIDE.md) | 日志格式、过滤、排障 |
| [common/COMMON.md](../common/COMMON.md) | 消息总表、IPC 拓扑、contract 四步 |
| `app/ui/UI.md` | UI Proxy 约定 |
| `app/glavo/GLAVO.md` | Glavo 后端示例 |
| `app/camera/CAMERA.md` | Camera 后端示例 |

---

## 9. 最小示例：新增 `rongsu_foo` 仅打日志、无新 IPC 消息

1. `upc_contract.hpp`：`APP_FOO = 0x04` + `process_config_foo()`
2. `upc_contract.hpp` §2：`UPC_MOD_FOO_SERVICE` + OFF 分支 `= 0`
3. `rs_log.hpp`：`UPC_LOG_FOO_SERVICE`
4. `foo/main.cpp`：`log_set_process_name` + `set_mode_ipc_client(APP_FOO)` + 空转 `process(50)`
5. `foo/CMakeLists.txt` + `foo/build.sh`（链 `upc::ipc`）
6. 同步 `3rdparty/commom/` → `./build.sh` → `scripts/rongsu.sh` 加启动项
7. `grep 'module=FooService' /var/log/rongsu_foo*.log` 验证

此示例不新增 `MsgType`，故无需改 UI Proxy 与其它进程 handler。

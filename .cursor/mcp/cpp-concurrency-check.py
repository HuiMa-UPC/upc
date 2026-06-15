#!/usr/bin/env python3
"""
rongsu_upc 项目 C++ 并发 / 内存序静态检查 MCP Server。

覆盖模块：ipc_shm_ring、ServiceBase、McuLink、slice_ring_buffer、
camera_preview、parser worker 等。规则为启发式，不能替代 TSan/审查。
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "cpp-concurrency-check"
SERVER_VERSION = "2.0.0"

# 项目内并发热点（相对仓库根目录）
DEFAULT_HOTSPOTS = [
    "common/ipc/ipc_shm_ring.cpp",
    "common/ipc/ipc_shm_ring.hpp",
    "common/ipc/ipc_shm_layout.hpp",
    "glavo/lib/service_base.cpp",
    "glavo/lib/glavo_service.cpp",
    "glavo/lib/mcu_link.cpp",
    "glavo/lib/print_job_state.cpp",
    "parser/slice_ring_buffer.cpp",
    "parser/slice_ring_buffer.hpp",
    "parser/gcode/gcode_worker.cpp",
    "ui/lib/core/src/camera_preview.cpp",
    "test/camera/test_camera_service.cpp",
]

VALID_SHM_TRANSITIONS = {
    ("Empty", "Writing"),
    ("Writing", "Ready"),
    ("Writing", "Empty"),
    ("Ready", "Reading"),
    ("Reading", "Empty"),
}


@dataclass
class Issue:
    severity: str  # error | warn | info
    category: str
    message: str
    line: Optional[int] = None

    def format(self) -> str:
        loc = f"L{self.line} " if self.line else ""
        icon = {"error": "🔴", "warn": "⚠️", "info": "ℹ️"}.get(self.severity, "•")
        return f"- {icon} [{self.category}] {loc}{self.message}"


def send(data: dict) -> None:
    print(json.dumps(data, ensure_ascii=False), flush=True)


def read_msg() -> Optional[dict]:
    line = sys.stdin.readline()
    if not line:
        return None
    line = line.strip()
    if not line:
        return read_msg()
    return json.loads(line)


def find_project_root(start: Path) -> Path:
    cur = start.resolve()
    for _ in range(8):
        if (cur / "CMakeLists.txt").is_file() and (cur / ".cursor").is_dir():
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return start.resolve()


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = find_project_root(SCRIPT_DIR.parent.parent)


def resolve_path(raw: str) -> Path:
    p = Path(raw)
    if p.is_absolute():
        return p
    return (PROJECT_ROOT / p).resolve()


def line_no(code: str, needle: str, occurrence: int = 0) -> Optional[int]:
    idx = -1
    for _ in range(occurrence + 1):
        idx = code.find(needle, idx + 1)
        if idx < 0:
            return None
    return code.count("\n", 0, idx) + 1


def strip_comments(code: str) -> str:
    """粗略去掉 // 与 /* */ 注释，降低误报。"""
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", code)


def analyze_ipc_shm(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []
    if "IpcShm" not in code and "ipc_shm" not in code:
        return issues

    body = strip_comments(code)

    if "compare_exchange_strong" in body:
        if "memory_order_acq_rel" not in body:
            issues.append(Issue(
                "error", "ipc_shm",
                "CAS (compare_exchange_strong) 应显式使用 memory_order_acq_rel / acquire 配对",
                line_no(code, "compare_exchange_strong"),
            ))
        for m in re.finditer(
            r"compare_exchange_strong\s*\([^)]*\)",
            body,
            re.DOTALL,
        ):
            snippet = m.group(0)
            if "memory_order" not in snippet:
                ln = code.count("\n", 0, m.start()) + 1
                issues.append(Issue(
                    "warn", "ipc_shm",
                    "compare_exchange_strong 未显式指定 memory_order（默认为 seq_cst，SHM 热路径建议 acq_rel）",
                    ln,
                ))

    # 消费者扫描 Ready 帧时的 seq/state load
    if "acquire_latest_frame" in body:
        frame_fn = re.search(
            r"acquire_latest_frame\s*\(\).*?(?=\n(?:std::|void|bool|\})|\Z)",
            body,
            re.DOTALL,
        )
        if frame_fn:
            fn = frame_fn.group(0)
            for m in re.finditer(
                r"seq\.load\(\s*(std::)?memory_order_([a-z_]+)\s*\)", fn
            ):
                if m.group(2) not in ("acquire", "seq_cst"):
                    ln = code.count("\n", 0, m.start()) + 1
                    issues.append(Issue(
                        "warn", "ipc_shm",
                        f"acquire_latest_frame 内 seq.load(memory_order_{m.group(2)}) 建议 acquire",
                        ln,
                    ))
            for m in re.finditer(
                r"state\.load\(\s*(std::)?memory_order_([a-z_]+)\s*\)", fn
            ):
                if m.group(2) not in ("acquire", "seq_cst"):
                    ln = code.count("\n", 0, m.start()) + 1
                    issues.append(Issue(
                        "warn", "ipc_shm",
                        f"acquire_latest_frame 内 state.load(memory_order_{m.group(2)}) 建议 acquire",
                        ln,
                    ))

    for m in re.finditer(
        r"IpcShmSlotState::(\w+)[^\n]*IpcShmSlotState::(\w+)", body
    ):
        pair = (m.group(1), m.group(2))
        if pair not in VALID_SHM_TRANSITIONS:
            ln = code.count("\n", 0, m.start()) + 1
            issues.append(Issue(
                "warn", "ipc_shm",
                f"可疑 SHM 槽位状态转换: {pair[0]} -> {pair[1]}",
                ln,
            ))

    # 调用 slot_payload()，而非头文件里的声明
    if re.search(r"slot_payload\s*\(", body) and "acquire_write_slot" not in body:
        if "IpcShmRingProducer::slot_payload" not in body:
            issues.append(Issue(
                "warn", "ipc_shm",
                "调用 slot_payload() 前应 acquire_write_slot()（仅 Writing 状态可写 payload）",
                line_no(code, "slot_payload("),
            ))

    if "commit_slot" in body and path.endswith(".cpp"):
        commit_fn = re.search(
            r"commit_slot\s*\([^)]*\)\s*\{.*?\n\}",
            body,
            re.DOTALL,
        )
        if commit_fn and "timestamp_us" not in commit_fn.group(0):
            issues.append(Issue(
                "info", "ipc_shm",
                "commit_slot 实现中应设置 timestamp_us，再 release 发布 seq/state",
                line_no(code, "commit_slot"),
            ))

    return issues


def analyze_service_base(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []
    if path.endswith("service_base.cpp") or path.endswith("service_base.hpp"):
        return issues

    body = strip_comments(code)
    if "handle_message" not in body and "worker_loop" not in body:
        return issues

    for m in re.finditer(r"(?<![_a-zA-Z])publish\s*\(", body):
        ctx = body[max(0, m.start() - 24):m.start()]
        if "publish_async" in ctx:
            continue
        ln = code.count("\n", 0, m.start()) + 1
        issues.append(Issue(
            "error", "service_base",
            "handle_message / 工作线程中调用 publish() 可能阻塞 MsgBus；回复 UI 应使用 publish_async()",
            ln,
        ))

    return issues


def analyze_mcu_link(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []
    if "McuLink" not in code and "link_thread_fn" not in code:
        return issues

    if "link_thread_fn" in code:
        if "tcp_send_mtx_" in code:
            fn = re.search(r"void McuLink::link_thread_fn\(\).*?(?=\n\w|\Z)", code, re.DOTALL)
            if fn and "send(" in fn.group(0) and "tcp_send_mtx_" not in fn.group(0):
                issues.append(Issue(
                    "warn", "mcu_link",
                    "link_thread_fn 内若有 TCP send，应持有 tcp_send_mtx_ 避免并发写 socket",
                    line_no(code, "link_thread_fn"),
                ))

    if re.search(r"std::atomic<bool>\s+\w+_", code) and ".load()" in code:
        for m in re.finditer(r"(\w+_)\.load\(\s*\)", code):
            ln = code.count("\n", 0, m.start()) + 1
            issues.append(Issue(
                "info", "mcu_link",
                f"{m.group(1)}load() 未指定 memory_order；控制标志可用 relaxed，跨线程协调建议 acquire",
                ln,
            ))

    return issues


def analyze_ring_buffer(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []
    if "RingBuffer" not in code and "slice_ring_buffer" not in path:
        return issues

    if path.endswith(".cpp") and "put_wait" in code and "stop_flag" in code:
        if "stop_flag.load(std::memory_order_relaxed)" not in code:
            issues.append(Issue(
                "info", "ring_buffer",
                "put_wait 轮询 stop_flag 建议使用 memory_order_relaxed（仅作停止信号）",
                line_no(code, "stop_flag"),
            ))

    if "rb_" in code or "RingBuffer" in code:
        if "put_wait" not in code and "put(" in code:
            issues.append(Issue(
                "warn", "ring_buffer",
                "parser→McuLink 路径上阻塞 put 可能卡死解析线程；stop 场景应优先 put_wait(stop_flag)",
                line_no(code, "put("),
            ))

    return issues


def analyze_camera_preview(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []
    if "CameraPreviewPlayer" not in code and "camera_preview" not in path:
        return issues

    if "display_buf_" in code:
        has_release = "display_buf_.store" in code and "memory_order_release" in code
        has_acquire = "display_buf_.load" in code and "memory_order_acquire" in code
        if not (has_release and has_acquire):
            issues.append(Issue(
                "warn", "camera_preview",
                "display_buf_ 跨解码线程/UI 线程传递索引，建议 store(release) + load(acquire) 配对",
                line_no(code, "display_buf_"),
            ))

    if "decode_thread_fn" in code:
        decode = re.search(r"decode_thread_fn\(\).*?(?=\nvoid CameraPreviewPlayer::|\Z)", code, re.DOTALL)
        if decode:
            body = decode.group(0)
            for pat in (r"lv_label_set_text", r"lv_obj_", r"lv_image_", r"lv_timer_"):
                if re.search(pat, body) and "lv_async_call" not in body:
                    issues.append(Issue(
                        "error", "camera_preview",
                        "decode_thread_fn 中直接调用 LVGL API 非线程安全；应 lv_async_call 切回 UI 线程",
                        line_no(body, pat),
                    ))
                    break

    return issues


def analyze_parser_worker(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []
    if "GcodeWorker" not in code and "OvfWorker" not in code and "parser_thread_fn" not in code:
        return issues

    if "std::thread" in code and "join()" not in code and "~" in code:
        issues.append(Issue(
            "warn", "parser_worker",
            "解析 worker 使用 std::thread 时，析构/stop 路径应 join()，避免 std::terminate",
            line_no(code, "std::thread"),
        ))

    if "stop_req_" in code and "stop_req_.store(true)" in code:
        if "join()" not in code:
            issues.append(Issue(
                "warn", "parser_worker",
                "stop_parse 设置 stop_req_ 后应 join 解析线程，确保 put_wait 被唤醒退出",
                line_no(code, "stop_req_.store(true)"),
            ))

    return issues


def analyze_generic(path: str, code: str) -> list[Issue]:
    issues: list[Issue] = []

    if path.endswith(".hpp") and "std::mutex" in code:
        cpp_sibling = path.replace(".hpp", ".cpp")
        sibling = PROJECT_ROOT / cpp_sibling
        if sibling.is_file():
            return issues

    body = strip_comments(code)
    if "std::mutex" in body and "lock_guard" not in body and "unique_lock" not in body:
        issues.append(Issue(
            "info", "generic",
            "存在 std::mutex 但未发现 lock_guard/unique_lock，请确认加锁方式",
        ))

    if re.search(r"std::thread\s+\w+.*detach\s*\(", code):
        issues.append(Issue(
            "warn", "generic",
            "std::thread::detach 使生命周期难追踪，项目内优先 join + 明确 stop 标志",
            line_no(code, "detach"),
        ))

    return issues


def analyze_file(path: Path) -> tuple[str, list[Issue]]:
    try:
        code = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return str(path), [Issue("error", "io", str(exc))]

    rel = str(path.relative_to(PROJECT_ROOT)) if path.is_relative_to(PROJECT_ROOT) else str(path)
    issues: list[Issue] = []
    for fn in (
        analyze_ipc_shm,
        analyze_service_base,
        analyze_mcu_link,
        analyze_ring_buffer,
        analyze_camera_preview,
        analyze_parser_worker,
        analyze_generic,
    ):
        issues.extend(fn(rel, code))

    # 去重（同 category + message + line）
    seen: set[tuple] = set()
    unique: list[Issue] = []
    for it in issues:
        key = (it.severity, it.category, it.message, it.line)
        if key not in seen:
            seen.add(key)
            unique.append(it)

    order = {"error": 0, "warn": 1, "info": 2}
    unique.sort(key=lambda x: (order.get(x.severity, 9), x.line or 0, x.category))
    return rel, unique


def format_report(paths_results: list[tuple[str, list[Issue]]]) -> str:
    total_errors = sum(1 for _, iss in paths_results for i in iss if i.severity == "error")
    total_warns = sum(1 for _, iss in paths_results for i in iss if i.severity == "warn")
    total_infos = sum(1 for _, iss in paths_results for i in iss if i.severity == "info")

    lines = [
        "# C++ 并发 / 内存序分析报告",
        "",
        f"项目根: `{PROJECT_ROOT}`",
        f"扫描文件: {len(paths_results)} | 🔴 {total_errors} | ⚠️ {total_warns} | ℹ️ {total_infos}",
        "",
        "> 启发式静态检查，不能替代 ThreadSanitizer 与 code review。",
        "",
    ]

    for rel, issues in paths_results:
        lines.append(f"## `{rel}`")
        if not issues:
            lines.append("✅ 未命中已知并发反模式")
        else:
            lines.extend(i.format() for i in issues)
        lines.append("")

    lines.extend([
        "## 建议验证命令",
        "",
        "```bash",
        "# 交叉工具链下若支持 TSan，可对单测/桩程序启用：",
        "clang++ -fsanitize=thread -g -O1 -pthread your_test.cpp -o tsan_test",
        "./tsan_test",
        "```",
        "",
        "重点人工审查：SHM slot 状态机、ServiceBase 工作线程回 UI、camera 解码线程与 LVGL。",
    ])
    return "\n".join(lines)


def tool_check_file(arguments: dict) -> str:
    raw = arguments.get("file_path", "")
    if not raw:
        raise ValueError("缺少 file_path")
    path = resolve_path(raw)
    if not path.is_file():
        raise FileNotFoundError(f"文件不存在: {path}")
    return format_report([analyze_file(path)])


def tool_scan_hotspots(arguments: dict) -> str:
    paths_arg = arguments.get("paths")
    if paths_arg:
        rel_paths = [p.strip() for p in paths_arg if p.strip()]
    else:
        rel_paths = list(DEFAULT_HOTSPOTS)

    results: list[tuple[str, list[Issue]]] = []
    missing: list[str] = []
    for rel in rel_paths:
        path = resolve_path(rel)
        if path.is_file():
            results.append(analyze_file(path))
        else:
            missing.append(rel)

    report = format_report(results)
    if missing:
        report += "\n## 未找到的文件\n\n" + "\n".join(f"- `{p}`" for p in missing)
    return report


TOOLS = {
    "check_file": {
        "description": "分析单个 C/C++ 源文件的并发安全与内存序（相对项目根或绝对路径）",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file_path": {
                    "type": "string",
                    "description": "例如 common/ipc/ipc_shm_ring.cpp",
                },
            },
            "required": ["file_path"],
        },
        "handler": tool_check_file,
    },
    "scan_hotspots": {
        "description": "扫描项目内并发热点文件（ipc_shm、ServiceBase、McuLink、RingBuffer、camera_preview、parser worker 等）",
        "inputSchema": {
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "可选，自定义文件列表；省略则使用内置热点列表",
                },
            },
        },
        "handler": tool_scan_hotspots,
    },
}


def handle_initialize(msg: dict) -> None:
    send({
        "jsonrpc": "2.0",
        "id": msg["id"],
        "result": {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        },
    })


def handle_tools_list(msg: dict) -> None:
    tools = []
    for name, spec in TOOLS.items():
        tools.append({
            "name": name,
            "description": spec["description"],
            "inputSchema": spec["inputSchema"],
        })
    send({"jsonrpc": "2.0", "id": msg["id"], "result": {"tools": tools}})


def handle_tools_call(msg: dict) -> None:
    req_id = msg["id"]
    params = msg.get("params") or {}
    name = params.get("name", "")
    arguments = params.get("arguments") or {}

    if name not in TOOLS:
        send({
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32602, "message": f"未知工具: {name}"},
        })
        return

    try:
        text = TOOLS[name]["handler"](arguments)
        send({
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {"content": [{"type": "text", "text": text}]},
        })
    except Exception as exc:
        send({
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "content": [{"type": "text", "text": f"❌ 执行失败: {exc}"}],
                "isError": True,
            },
        })


def dispatch(msg: dict) -> None:
    method = msg.get("method", "")
    if method == "initialize":
        handle_initialize(msg)
    elif method == "notifications/initialized":
        return
    elif method == "tools/list":
        handle_tools_list(msg)
    elif method == "tools/call":
        handle_tools_call(msg)
    elif method == "ping":
        send({"jsonrpc": "2.0", "id": msg.get("id"), "result": {}})
    elif "id" in msg and method.startswith("notifications/"):
        return
    elif "id" in msg:
        send({
            "jsonrpc": "2.0",
            "id": msg["id"],
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        })


def main() -> None:
    if hasattr(sys.stdin, "reconfigure"):
        sys.stdin.reconfigure(encoding="utf-8")
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    while True:
        msg = read_msg()
        if msg is None:
            break
        dispatch(msg)


if __name__ == "__main__":
    main()

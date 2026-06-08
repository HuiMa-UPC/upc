#!/usr/bin/env python3
"""
gen-commit-msg.py — 根据 git status 自动生成规范化提交日志

用法:
    python3 scripts/gen-commit-msg.py              # 分析 working tree + 暂存区
    python3 scripts/gen-commit-msg.py --cached     # 仅分析暂存区
    python3 scripts/gen-commit-msg.py -o msg.txt   # 输出到文件
    python3 scripts/gen-commit-msg.py --edit       # 生成后用编辑器打开

依赖: Python 3.7+
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Tuple


# ---------------------------------------------------------------------------
# 规则配置：文件路径 → 改动类别（中文标题 + 英文 type）
# 前面的规则优先匹配
# ---------------------------------------------------------------------------
PATH_RULES: List[Tuple[str, str, str]] = [
    # 构建系统
    (r"(^|/)CMakeLists\.txt$", "构建配置", "build"),
    (r"(^|/)cmake/.*", "构建配置", "build"),
    (r"(^|/)Makefile$", "构建配置", "build"),
    (r"(^|/)\.gitattributes$", "构建配置", "build"),
    (r"(^|/)\.gitignore$", "构建配置", "build"),
    # 第三方依赖
    (r"(^|/)3rdparty/.*", "第三方依赖", "build"),
    # 文档
    (r"(^|/)doc/.*", "文档", "docs"),
    (r"(^|/)README.*", "文档", "docs"),
    (r"(^|/)AGENTS\.md$", "文档", "docs"),
    (r"^[^/]*\.md$", "文档", "docs"),
    # 公共基础设施
    (r"(^|/)common/debug/.*", "日志与监控", "feat"),
    (r"(^|/)common/contract/.*", "消息契约", "feat"),
    (r"(^|/)common/middleware/.*", "中间件", "feat"),
    (r"(^|/)common/ipc/.*", "进程通信", "feat"),
    (r"(^|/)common/process/.*", "进程管理", "feat"),
    (r"(^|/)common/.*", "公共基础设施", "feat"),
    # UI
    (r"(^|/)ui/.*", "UI", "feat"),
    # Glavo / 后端
    (r"(^|/)glavo/.*", "后端服务", "feat"),
    # 脚本
    (r"(^|/)scripts/.*", "脚本", "chore"),
    # 应用入口
    (r"(^|/)app/.*", "应用入口", "feat"),
    # 默认兜底
    (r".*", "其他", "chore"),
]

# 从单文件 diff 中提取语义描述的关键词规则
DIFF_KEYWORDS: List[Tuple[str, str]] = [
    (r"\+.*log_set_process_name", "新增进程名标记，日志增加 proc= 字段"),
    (r"\+.*upc_sys_monitor_tick", "接入系统监控心跳"),
    (r"\+.*serial_log_frame_hex", "增加串口帧十六进制转储"),
    (r"\+.*\.gitattributes", "新增 .gitattributes，统一 LF 行尾符"),
    (r"-\s*find_library\(_EVDEV_LIB", "移除 libevdev 依赖查找"),
    (r"-\s*mali_hook", "移除 mali_hook 库链接"),
    (r"TOOLCHAIN\s+k1mini", "切换默认工具链为 k1mini"),
    (r"TOOLCHAIN\s+atk-dlrk356x", "增加 atk-dlrk356x 工具链选项"),
    (r"\+.*k1mini-buildroot-toolchain", "新增 k1mini 工具链配置"),
    (r"\+.*atk-dlrk356x-toolchain", "新增 atk-dlrk356x 工具链配置"),
]

# 类别输出顺序
CAT_ORDER = [
    "构建配置", "第三方依赖", "公共基础设施", "日志与监控",
    "消息契约", "中间件", "进程通信", "进程管理",
    "UI", "后端服务", "应用入口", "脚本", "文档", "其他",
]


def run_git(args: List[str]) -> str:
    """执行 git 命令，返回 stdout。"""
    result = subprocess.run(
        ["git"] + args,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        print(f"git error: {result.stderr.strip()}", file=sys.stderr)
        sys.exit(1)
    return result.stdout


def get_status_entries(cached: bool) -> List[Tuple[str, str]]:
    """
    获取文件状态列表。
    返回 [(status_code, filepath), ...]
    status_code: M(修改) A(新增) D(删除) R(重命名) ?(未跟踪)
    """
    entries = []

    if cached:
        # 仅暂存区：git diff --cached --name-status -z
        raw = run_git(["diff", "--cached", "--name-status", "-z"])
        parts = raw.split("\x00")
        i = 0
        while i < len(parts):
            if not parts[i]:
                i += 1
                continue
            status = parts[i][0]  # 取首字母
            if status in ("R", "C"):
                if i + 2 < len(parts):
                    entries.append((status, parts[i + 2]))
                    i += 3
                    continue
            else:
                if i + 1 < len(parts):
                    entries.append((status, parts[i + 1]))
                    i += 2
                    continue
            i += 1
    else:
        # 工作区完整状态：git status --porcelain -z --no-renames
        raw = run_git(["status", "--porcelain", "-z", "--no-renames"])
        parts = raw.split("\x00")
        i = 0
        while i < len(parts):
            if not parts[i] or len(parts[i]) < 2:
                i += 1
                continue
            # porcelain 格式: XY filename
            # X = index status, Y = working tree status
            xy = parts[i]
            x, y = xy[0], xy[1]
            fname = parts[i][3:] if len(parts[i]) > 3 else ""

            # 确定最终状态（index 优先于 working tree）
            if x == "?" and y == "?":
                status = "?"
            elif x in ("A", "M", "D", "R", "C"):
                status = x
            elif y in ("M", "D"):
                status = y
            else:
                i += 1
                continue

            if fname:
                entries.append((status, fname))
            i += 1

    return entries


def get_file_diff(filepath: str, cached: bool) -> str:
    """获取单文件的 diff 文本（用于语义分析）。未跟踪文件返回空。"""
    if filepath.startswith("?"):
        return ""
    args = ["diff"]
    if cached:
        args.append("--cached")
    args.extend(["--", filepath])
    return run_git(args)


def classify_file(filepath: str) -> Tuple[str, str]:
    """根据路径规则返回 (中文类别, 英文 type)。"""
    for pattern, cn_title, en_type in PATH_RULES:
        if re.search(pattern, filepath):
            return cn_title, en_type
    return "其他", "chore"


def extract_semantic_hint(filepath: str, diff_text: str) -> str:
    """从单文件 diff 中提取语义提示。"""
    for pattern, hint in DIFF_KEYWORDS:
        if re.search(pattern, diff_text, re.MULTILINE):
            return hint
    return ""


def pick_primary_type(grouped: Dict[str, List[dict]]) -> str:
    """根据改动量选择主 type。"""
    type_scores: Dict[str, int] = {}
    type_priority = {
        "feat": 3, "fix": 3, "build": 2, "docs": 1, "chore": 0,
        "refactor": 2, "test": 1, "style": 0,
    }
    for files in grouped.values():
        for f in files:
            t = f["en_type"]
            score = type_priority.get(t, 0) * 10 + 1
            type_scores[t] = type_scores.get(t, 0) + score
    if not type_scores:
        return "chore"
    return max(type_scores, key=lambda k: (type_scores[k], type_priority.get(k, 0)))


def pick_subject(grouped: Dict[str, List[dict]], primary_type: str) -> str:
    """生成一行英文 subject。"""
    # 收集所有语义提示
    all_hints = []
    for files in grouped.values():
        for f in files:
            h = f.get("hint", "")
            if h:
                all_hints.append(h)

    # 根据提示生成 subject
    hint_str = " ".join(all_hints).lower()

    if "lf" in hint_str or "gitattributes" in hint_str:
        subject = "enforce LF line endings and update build config"
    elif "toolchain" in hint_str or "k1mini" in hint_str:
        subject = "switch toolchain to k1mini and cleanup build artifacts"
    elif "log_set_process_name" in hint_str or "proc=" in hint_str:
        subject = "add process name to log output and system monitor"
    elif "serial_log_frame_hex" in hint_str:
        subject = "add serial frame hex dump"
    elif "libevdev" in hint_str:
        subject = "remove libevdev dependency and update link flags"
    else:
        cats = list(grouped.keys())
        if len(cats) == 1:
            subject = f"update {cats[0].lower()}"
        elif len(cats) == 2:
            subject = f"update {cats[0].lower()} and {cats[1].lower()}"
        else:
            subject = "update multiple modules"

    # 限制长度在 72 以内
    if len(subject) > 72:
        subject = subject[:69] + "..."
    return subject


def generate_commit_msg(cached: bool) -> str:
    status_entries = get_status_entries(cached)
    if not status_entries:
        return "# 暂无可提交的更改\n"

    # 按类别分组
    grouped: Dict[str, List[dict]] = {}
    for scode, fpath in status_entries:
        cn_cat, en_type = classify_file(fpath)
        diff_text = get_file_diff(fpath, cached) if scode in ("M", "A", "R", "C") else ""
        hint = extract_semantic_hint(fpath, diff_text)

        entry = {
            "status": scode,
            "path": fpath,
            "en_type": en_type,
            "hint": hint,
        }
        grouped.setdefault(cn_cat, []).append(entry)

    primary_type = pick_primary_type(grouped)
    subject = pick_subject(grouped, primary_type)

    lines: List[str] = []
    lines.append(f"{primary_type}: {subject}")
    lines.append("")

    for cat in CAT_ORDER:
        if cat not in grouped:
            continue
        lines.append(cat)
        files = grouped[cat]
        for e in files:
            scode = e["status"]
            path = e["path"]
            hint = e.get("hint", "")

            # 状态符号
            sym_map = {"A": "+", "D": "-", "M": "~", "R": "→", "C": "→", "?": "?"}
            prefix = sym_map.get(scode, "?")

            if hint:
                lines.append(f"- {prefix} {path}: {hint}")
            else:
                lines.append(f"- {prefix} {path}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="根据 git status 自动生成规范化提交日志"
    )
    parser.add_argument(
        "--cached", action="store_true",
        help="仅分析暂存区（git diff --cached）"
    )
    parser.add_argument(
        "-o", "--output", metavar="FILE",
        help="输出到文件（默认打印到 stdout）"
    )
    parser.add_argument(
        "--edit", action="store_true",
        help="生成后调用 git 默认编辑器编辑"
    )
    args = parser.parse_args()

    msg = generate_commit_msg(args.cached)
    if "暂无可提交的更改" in msg:
        print(msg, end="")
        return 1

    if args.output:
        Path(args.output).write_text(msg, encoding="utf-8")
        print(f"提交日志已写入: {args.output}", file=sys.stderr)
        if args.edit:
            editor_out = subprocess.run(
                ["git", "var", "GIT_EDITOR"], capture_output=True, text=True
            )
            ed = editor_out.stdout.strip() or "vi"
            subprocess.run([ed, args.output])
    else:
        print(msg, end="")
        if args.edit:
            with tempfile.NamedTemporaryFile(
                mode="w", suffix=".txt", delete=False, encoding="utf-8"
            ) as f:
                f.write(msg)
                tmp = f.name
            editor_out = subprocess.run(
                ["git", "var", "GIT_EDITOR"], capture_output=True, text=True
            )
            ed = editor_out.stdout.strip() or "vi"
            subprocess.run([ed, tmp])
            final = Path(tmp).read_text(encoding="utf-8")
            print(final, end="")
            Path(tmp).unlink()

    return 0


if __name__ == "__main__":
    sys.exit(main())

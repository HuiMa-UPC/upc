#!/usr/bin/env python3
"""
只读查询 UPC IPC 契约：MsgType / Payload / COMMON.md 总表 / 报警 CSV。

新增消息时改 upc_contract.hpp + COMMON.md 四步即可，不必改本脚本。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from _mcp_stdio import run_stdio  # noqa: E402
from upc_contract_index import (  # noqa: E402
    ALARM_CSV_REL,
    HPP_REL,
    MD_REL,
    REQ_NO_DEDICATED_RSP,
    SHARED_RSP,
    alarm_snapshot_code_of,
    alarm_snapshot_slot_bit,
    find_project_root,
    get_index,
    guess_rsp_names,
    payload_for_msg,
)

SERVER_NAME = "upc-dev"
SERVER_VERSION = "1.0.0"
PROJECT_ROOT = find_project_root(SCRIPT_DIR.parent.parent)

FOUR_STEPS = (
    "改消息固定四步：1) `upc_contract.hpp` 在**所在分段末尾**追加 MsgType+POD"
    "（禁止中段插入） 2) 发送/接收进程 subscribe/publish 3) UI 若需要则"
    "`send_*`/`register_*` 4) `COMMON.md` 总表补一行。回 UI 用 `publish_async()`。"
)


def _idx():
    return get_index(PROJECT_ROOT)


def _parse_query_value(q: str) -> int | None:
    s = q.strip()
    try:
        if s.lower().startswith("0x"):
            return int(s, 16)
        if s.isdigit():
            v = int(s)
            if v >= 0x1000:
                return v
    except ValueError:
        return None
    return None


def _find_msgs(query: str):
    idx = _idx()
    q = query.strip()
    if not q:
        return []
    val = _parse_query_value(q)
    if val is not None and val in idx.by_value:
        return [idx.msgs[idx.by_value[val]]]

    if q in idx.msgs:
        return [idx.msgs[q]]

    ql = q.lower()
    hits = []
    for e in idx.msgs.values():
        if ql == e.name.lower() or (e.canonical and ql == e.canonical.lower()):
            hits.append(e)
    if hits:
        return hits
    for e in idx.msgs.values():
        if ql in e.name.lower() or (e.canonical and ql in e.canonical.lower()):
            hits.append(e)
    return hits


def _format_fields(struct) -> str:
    if not struct.fields:
        return "（无解析到的字段）"
    lines = []
    for typ, name in struct.fields:
        lines.append(f"  - `{typ} {name}`")
    return "\n".join(lines)


def _format_msg(entry) -> str:
    idx = _idx()
    lines = [
        f"# `{entry.name}` = `{entry.value:#06x}` ({entry.value})",
        "",
        f"- 头文件：`{HPP_REL}:{entry.line}`",
        f"- 分段：{entry.segment or '（无分段注释）'}",
        f"- 后缀：{entry.suffix or '（非 Req/Rsp/Ntf/Done）'}",
    ]
    if entry.canonical:
        lines.append(f"- 规范名：`{entry.canonical}`")
    if entry.comment:
        lines.append(f"- 注释：{entry.comment}")
    flags = []
    if entry.name in idx.reliable:
        flags.append("可靠补发白名单 `is_ipc_reliable_msg_type`")
    if entry.name in idx.latest_only:
        flags.append("断连只留最新 `is_ipc_latest_only_msg_type`")
    if flags:
        lines.append("- IPC 策略：" + "；".join(flags))

    if entry.md:
        md = entry.md
        lines.extend([
            "",
            f"## 总表（`{MD_REL}`）",
            f"- 方向：{md['direction']}",
            f"- Payload：{md['payload']}",
            f"- UI API：{md['ui_api']}",
            f"- 处理：{md['handler']}",
        ])
    else:
        lines.extend(["", f"## 总表", f"- `{MD_REL}` **无此枚举行**（`check_contract_drift` 会报）。"])

    structs = payload_for_msg(idx, entry)
    if structs:
        lines.append("")
        lines.append("## Payload 字段")
        for st in structs:
            lines.append(f"### `{st.name}`（`{HPP_REL}:{st.line}`）")
            if st.doc:
                lines.append(st.doc)
            lines.append(_format_fields(st))
    elif entry.payload_names:
        lines.append("")
        lines.append("## Payload")
        lines.append("注释/总表提到：" + ", ".join(f"`{n}`" for n in entry.payload_names))

    if entry.suffix == "Req":
        rsps = guess_rsp_names(entry.name, idx.msgs)
        lines.append("")
        lines.append("## 配对 Rsp")
        if rsps:
            for r in rsps:
                re = idx.msgs[r]
                extra = "（Pause/Resume 共用）" if entry.name in SHARED_RSP else ""
                lines.append(f"- `{r}` = `{re.value:#06x}`{extra}")
        elif entry.name in REQ_NO_DEDICATED_RSP:
            lines.append("- 已知无独立 `*_Rsp`（状态走 Ntf / 进度推送）。")
        else:
            lines.append("- 未找到对应 `*_Rsp`。")

    lines.extend(["", "## 维护", FOUR_STEPS])
    return "\n".join(lines)


def tool_lookup_msg(arguments: dict) -> str:
    query = (arguments.get("query") or "").strip()
    if not query:
        raise ValueError("缺少 query（枚举名、规范名或 0x1040）")
    hits = _find_msgs(query)
    if not hits:
        return f"未找到与 `{query}` 匹配的 MsgType。可用 `list_msgs` 按前缀浏览。"
    if len(hits) > 12:
        names = "\n".join(f"- `{e.name}` `{e.value:#06x}`" for e in hits[:40])
        return f"匹配过多（{len(hits)}），请写更具体的名字：\n{names}"
    if len(hits) == 1:
        return _format_msg(hits[0])
    parts = [f"匹配 {len(hits)} 条，请用全名再查：\n"]
    for e in hits:
        parts.append(f"- `{e.name}` `{e.value:#06x}` {e.segment}")
    parts.append("")
    parts.append(_format_msg(hits[0]))
    return "\n".join(parts)


def tool_list_msgs(arguments: dict) -> str:
    idx = _idx()
    prefix = (arguments.get("prefix") or "").strip()
    segment = (arguments.get("segment") or "").strip().lower()
    rows = []
    for e in idx.msgs.values():
        if prefix and not e.name.startswith(prefix) and not (
            e.canonical and e.canonical.startswith(prefix)
        ):
            continue
        if segment and segment not in e.segment.lower():
            continue
        rows.append(e)
    rows.sort(key=lambda x: x.value)
    if not rows:
        return "无匹配。prefix 例：`UI_Print_`、`GLAVO_Alarm_`；segment 例：`报警`、`打印流程`。"
    lines = [f"共 {len(rows)} 条（按线协议数值）\n"]
    cur_seg = None
    for e in rows:
        if e.segment != cur_seg:
            cur_seg = e.segment
            lines.append(f"### {cur_seg or '（无分段）'}")
        pl = ",".join(e.payload_names) if e.payload_names else ""
        extra = f"  {pl}" if pl else ""
        lines.append(f"- `{e.value:#06x}` `{e.name}`{extra}")
    return "\n".join(lines)


def tool_lookup_payload(arguments: dict) -> str:
    idx = _idx()
    name = (arguments.get("name") or "").strip()
    if not name:
        raise ValueError("缺少 name（结构体名，如 AlarmReportPayload）")
    st = idx.payloads.get(name)
    if not st:
        ql = name.lower()
        cands = [n for n in idx.payloads if ql in n.lower()]
        if not cands:
            return f"未找到 `{name}`。结构体需在 `{HPP_REL}` 且名以 Payload 结尾。"
        if len(cands) > 1:
            return "多个匹配：\n" + "\n".join(f"- `{n}`" for n in sorted(cands))
        st = idx.payloads[cands[0]]
        name = st.name

    used = []
    for e in idx.msgs.values():
        if name in e.payload_names:
            used.append(e)
        elif e.md and name in (e.md.get("payload") or ""):
            used.append(e)
    lines = [
        f"# `{st.name}`",
        f"- `{HPP_REL}:{st.line}`",
    ]
    if st.doc:
        lines.append(st.doc)
    lines.append("")
    lines.append("## 字段")
    lines.append(_format_fields(st))
    lines.append("")
    lines.append("## 使用该 Payload 的 MsgType")
    if not used:
        lines.append("（注释/总表未点名；可能是内嵌数组元素，如 `FileEntryPayload`）")
    else:
        for e in used:
            lines.append(f"- `{e.name}` `{e.value:#06x}`")
    return "\n".join(lines)


def tool_check_contract_drift(arguments: dict) -> str:
    idx = _idx()
    issues: list[str] = []

    hpp_only = sorted(n for n in idx.msgs if n not in idx.md_names)
    md_only = sorted(n for n in idx.md_names if n not in idx.msgs)
    if hpp_only:
        issues.append("### hpp 有、总表无")
        issues.extend(f"- `{n}` `{idx.msgs[n].value:#06x}`" for n in hpp_only)
        issues.append("")
    if md_only:
        issues.append("### 总表有、hpp 无")
        issues.extend(f"- `{n}`" for n in md_only)
        issues.append("")

    missing_rsp = []
    for e in idx.msgs.values():
        if e.suffix != "Req":
            continue
        if e.name in REQ_NO_DEDICATED_RSP:
            continue
        rsps = guess_rsp_names(e.name, idx.msgs)
        if not rsps:
            missing_rsp.append(e)
    if missing_rsp:
        issues.append("### `*_Req` 未找到对应 `*_Rsp`")
        issues.append("（已知无独立 Rsp 的已排除；Pause/Resume 共用已计入。）")
        for e in missing_rsp:
            issues.append(f"- `{e.name}` `{e.value:#06x}`")
        issues.append("")

    dups = []
    seen_val: dict[int, str] = {}
    for e in idx.msgs.values():
        if e.value in seen_val and seen_val[e.value] != e.name:
            dups.append((e.value, seen_val[e.value], e.name))
        seen_val[e.value] = e.name
    if dups:
        issues.append("### 线协议数值冲突")
        for v, a, b in dups:
            issues.append(f"- `{v:#06x}` `{a}` vs `{b}`")
        issues.append("")

    head = [
        f"# 契约漂移（对照 `{HPP_REL}` 与 `{MD_REL}` 总表）",
        "",
        f"- MsgType {len(idx.msgs)} 条；总表点名 {len(idx.md_names)} 个标识符。",
        "",
        FOUR_STEPS,
        "",
    ]
    if not issues:
        return "\n".join(head) + "✅ 未发现 hpp/总表标识符不一致，且每个 `_Req` 都能配上 `_Rsp`（含已知例外）。"
    return "\n".join(head + issues)


def tool_lookup_alarm(arguments: dict) -> str:
    idx = _idx()
    query = (arguments.get("query") or "").strip()
    if not query:
        raise ValueError("缺少 query（报警号、宏或中文名）")

    q = query.upper().replace(" ", "")
    hits = []
    code_i = None
    try:
        code_i = int(query.strip(), 0)
    except ValueError:
        code_i = None

    for row in idx.alarms:
        if code_i is not None and row["code"] == code_i:
            hits.append(row)
            continue
        if q == row["macro"].upper() or query in row["name"] or query in row["description"]:
            hits.append(row)

    extra_formula = ""
    if code_i is not None:
        sb = alarm_snapshot_slot_bit(code_i)
        if sb:
            extra_formula = f"- 快照槽：`slot_id={sb[0]}` `bit={sb[1]}`（公式 `alarm_snapshot_code_of`，勿用 CSV index 反推）\n"
            back = alarm_snapshot_code_of(sb[0], sb[1])
            extra_formula += f"- 回算校验：`alarm_snapshot_code_of({sb[0]}, {sb[1]}) = {back}`\n"

    if not hits:
        msg = f"CSV `{ALARM_CSV_REL}` 未命中 `{query}`。\n"
        if extra_formula:
            msg += "该号仍可能在快照范围内：\n" + extra_formula
            msg += "线上 severity/clear_mode 以 `AlarmReportPayload` 为准，字典可能落后 MCU。\n"
        return msg

    lines = [f"字典：`{ALARM_CSV_REL}`", extra_formula, "线上字段以 `GLAVO_Alarm_Report_Ntf` / `AlarmReportPayload` 为准，勿只信表。", ""]
    for row in hits[:15]:
        sb = alarm_snapshot_slot_bit(row["code"])
        slot = f"slot={sb[0]} bit={sb[1]}" if sb else "不在快照 1000..1395 组内前 96 号"
        lines.extend([
            f"## {row['code']} `{row['macro']}` {row['name']}",
            f"- kind={row['kind']} severity={row['severity']} clear_mode={row['clear_mode']}",
            f"- {slot}",
            f"- {row['description']}",
            "",
        ])
    return "\n".join(lines)


def _search_needle(needle: str) -> list[str]:
    root = str(PROJECT_ROOT)
    try:
        proc = subprocess.run(
            ["rg", "-n", "--glob", "*.{cpp,hpp,h,cc}", "--", needle, root],
            capture_output=True,
            text=True,
            timeout=20,
        )
        out = proc.stdout.strip()
        return out.splitlines() if out else []
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    try:
        proc = subprocess.run(
            [
                "grep", "-RIn",
                "--include=*.cpp", "--include=*.hpp", "--include=*.h", "--include=*.cc",
                "--", needle, root,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        out = proc.stdout.strip()
        return out.splitlines() if out else []
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    hits = []
    for path in PROJECT_ROOT.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h", ".cc"}:
            continue
        if any(p in path.parts for p in ("3rdparty", ".git", "output", "build")):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        rel = path.relative_to(PROJECT_ROOT)
        for i, line in enumerate(text.splitlines(), 1):
            if needle in line:
                hits.append(f"{rel}:{i}:{line.strip()}")
                if len(hits) >= 40:
                    return hits
    return hits


def tool_find_msg_handlers(arguments: dict) -> str:
    query = (arguments.get("query") or "").strip()
    if not query:
        raise ValueError("缺少 query")
    hits = _find_msgs(query)
    if not hits:
        return f"未知 MsgType `{query}`"
    entry = hits[0]
    needles = [f"MsgType::{entry.name}"]
    if entry.md:
        for tok in re.findall(r"`([^`]+)`", " ".join([entry.md["ui_api"], entry.md["handler"]])):
            if tok.startswith("send_") or tok.startswith("register_") or tok.startswith("handle_"):
                needles.append(tok)
        for tok in re.findall(
            r"\b(send_\w+|register_\w+|handle_\w+)\b",
            entry.md["ui_api"] + " " + entry.md["handler"],
        ):
            needles.append(tok)
    needles = list(dict.fromkeys(needles))

    lines = [f"# `{entry.name}` 源码引用（启发式）", ""]
    root_s = str(PROJECT_ROOT) + "/"
    for needle in needles:
        found = _search_needle(needle)
        lines.append(f"## `{needle}`")
        if not found:
            lines.append("（无）")
        else:
            for ln in found[:30]:
                if ln.startswith(root_s):
                    ln = ln[len(root_s):]
                lines.append(f"- `{ln}`")
            if len(found) > 30:
                lines.append(f"- … 共 {len(found)} 行")
        lines.append("")
    return "\n".join(lines)


TOOLS = {
    "lookup_msg": {
        "description": (
            "查询一条 IPC MsgType：线协议数值、分段、Payload 字段、COMMON.md 总表方向/API、配对 Rsp。"
            "改消息或分析某条 IPC 时用。query 为枚举名、规范名或 0x1040。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "例如 UI_Print_Start_Req、GLAVO_Alarm_Report_Ntf、0x1040",
                },
            },
            "required": ["query"],
        },
        "handler": tool_lookup_msg,
    },
    "list_msgs": {
        "description": "按前缀或分段列出 MsgType 短名单（名+数值+Payload 提示），不要一次拉全表。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "prefix": {"type": "string", "description": "如 UI_Print_、APP_Camera_、GLAVO_Alarm_"},
                "segment": {"type": "string", "description": "分段注释子串，如 打印流程、报警、WiFi"},
            },
        },
        "handler": tool_list_msgs,
    },
    "lookup_payload": {
        "description": "查询 upc_contract.hpp 中 Payload 结构体字段，以及哪些 MsgType 使用它。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "如 PrintStartReqPayload、AlarmReportPayload"},
            },
            "required": ["name"],
        },
        "handler": tool_lookup_payload,
    },
    "check_contract_drift": {
        "description": (
            "对照 upc_contract.hpp 的 MsgType 与 COMMON.md 消息总表：hpp/文档缺失、Req 无 Rsp、数值冲突。"
            "新增或改消息后先跑此项。"
        ),
        "inputSchema": {"type": "object", "properties": {}},
        "handler": tool_check_contract_drift,
    },
    "lookup_alarm": {
        "description": (
            "查报警 CSV 与快照 slot/bit 公式（alarm_snapshot_code_of）。"
            "分析 GLAVO_Alarm_Report_Ntf / Snapshot 时用。线上等级以报文为准。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "报警号 1003、宏 PRINTING_CABIN_DOOR_OPENED 或中文名"},
            },
            "required": ["query"],
        },
        "handler": tool_lookup_alarm,
    },
    "find_msg_handlers": {
        "description": "在源码中搜索 MsgType::名字 以及总表里的 send_/register_/handle_（启发式，非完整调用图）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "MsgType 名或 0x 值"},
            },
            "required": ["query"],
        },
        "handler": tool_find_msg_handlers,
    },
}


if __name__ == "__main__":
    run_stdio(TOOLS, SERVER_NAME, SERVER_VERSION)

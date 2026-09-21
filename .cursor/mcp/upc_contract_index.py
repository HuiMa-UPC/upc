#!/usr/bin/env python3
"""解析 upc_contract.hpp + COMMON.md 消息总表，供 upc-dev MCP 只读查询。"""

from __future__ import annotations

import csv
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

HPP_REL = "common/upc_contract.hpp"
MD_REL = "common/COMMON.md"
ALARM_CSV_REL = "doc/主控板串口通信协议 - 报警&事件表.csv"

# 两个 Req 共用一个 Rsp（契约注释写明）
SHARED_RSP = {
    "UI_Print_Pause_Req": "APP_Print_Pause_Rsp",
    "UI_Print_Resume_Req": "APP_Print_Pause_Rsp",
}

# 以 _Req 结尾但主应答不是 *_Rsp（走 Ntf / 状态推送）
REQ_NO_DEDICATED_RSP = {
    "UI_Print_Start_Req",  # 进度 APP_Print_Progress_Ntf / 拒绝 APP_Print_Start_Rejected_Ntf
    "UI_Print_Powder_Low_Ignore_Req",
    "UI_Mcu_Ota_Start_Req",  # APP_Mcu_Ota_Status_Ntf
    "UI_Serial_Mcu_Ota_Start_Req",
    "UI_Print_Video_Finalize_Req",
    "UI_Transport_Req",  # APP_Transport_Result_Ntf
    "UI_Ota_User_Decision_Req",
    "UI_Wifi_Enable_Req",
    "UI_Wifi_Info_Req",
    "UI_Wifi_Forget_Req",
    "UI_Wifi_Disconnect_Req",
}

# 最长前缀优先：Req 域名 → 可能的 Rsp 域名前缀
REQ_PREFIX_TO_RSP = (
    ("MQTT_Media_", ("APP_Mqtt_",)),
    ("GLAVO_Visual_", ("APP_Visual_",)),
    ("UI_Glavo_", ("APP_Glavo_",)),
    ("VISUAL_", ("APP_Visual_",)),
    ("MEDIA_", ("APP_Media_",)),
    ("UI_", ("APP_", "GLAVO_")),
    ("Fs_", ("Fs_",)),
    ("GLAVO_", ("APP_", "GLAVO_")),
)

RE_ENUM_MSGTYPE = re.compile(
    r"enum\s+class\s+MsgType\s*:\s*uint32_t\s*\{(.*?)\n\};",
    re.DOTALL,
)
RE_ENUM_MODULE = re.compile(
    r"enum\s+class\s+ModuleId\s*:\s*uint32_t\s*\{(.*?)\n\};",
    re.DOTALL,
)
RE_MEMBER = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*(0x[0-9A-Fa-f]+|\d+))?\s*,?"
    r"\s*(?:/\*\*(.*?)\*/)?",
    re.DOTALL,
)
RE_SEGMENT = re.compile(r"^\s*//\s*----\s*(.*?)\s*----\s*$")
RE_CANONICAL = re.compile(r"\[([A-Za-z_][A-Za-z0-9_]*)\]")
RE_PAYLOAD_TOKEN = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*Payload)\b")
RE_STRUCT = re.compile(
    r"struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{(.*?)\n\};",
    re.DOTALL,
)
RE_FIELD = re.compile(
    r"^\s*((?:const\s+)?[A-Za-z_][\w:]*(?:\s*<[^;{]+>)?)\s+"
    r"([A-Za-z_]\w*)(\[[^\]]+\])?\s*;",
)
RE_RELIABLE_FN = re.compile(
    r"is_ipc_reliable_msg_type\s*\(MsgType\s+type\)[^{]*\{(.*?)\n\}",
    re.DOTALL,
)
RE_LATEST_FN = re.compile(
    r"is_ipc_latest_only_msg_type\s*\(MsgType\s+type\)[^{]*\{(.*?)\n\}",
    re.DOTALL,
)
RE_CASE_MSG = re.compile(r"case\s+MsgType::([A-Za-z_][A-Za-z0-9_]*)\s*:")
RE_TICK = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)`")


@dataclass
class PayloadStruct:
    name: str
    line: int
    doc: str
    fields: list[tuple[str, str]]  # (type, name)


@dataclass
class MsgEntry:
    name: str
    value: int
    line: int
    segment: str
    comment: str
    canonical: str
    payload_names: list[str]
    suffix: str
    md: Optional[dict] = None


@dataclass
class ContractIndex:
    root: Path
    hpp_mtime: float
    md_mtime: float
    csv_mtime: float
    msgs: dict[str, MsgEntry] = field(default_factory=dict)
    by_value: dict[int, str] = field(default_factory=dict)
    modules: dict[str, tuple[int, str]] = field(default_factory=dict)
    payloads: dict[str, PayloadStruct] = field(default_factory=dict)
    reliable: set[str] = field(default_factory=set)
    latest_only: set[str] = field(default_factory=set)
    md_names: set[str] = field(default_factory=set)
    alarms: list[dict] = field(default_factory=list)


def find_project_root(start: Path) -> Path:
    cur = start.resolve()
    for _ in range(8):
        if (cur / "CMakeLists.txt").is_file() and (cur / ".cursor").is_dir():
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return start.resolve()


def _suffix(name: str) -> str:
    for s in ("_Req", "_Rsp", "_Ntf", "_Done"):
        if name.endswith(s):
            return s[1:]
    return ""


def _parse_enum_body(body: str, hpp_prefix_line: int) -> list[MsgEntry]:
    entries: list[MsgEntry] = []
    current = None
    segment = ""
    # hpp_prefix_line = `enum class MsgType` 所在行；`{` 同行，capture 以换行开头
    offset = 0
    for raw_line in body.splitlines(True):
        line_no = hpp_prefix_line + body[:offset].count("\n")
        offset += len(raw_line)
        line = raw_line.rstrip("\n")
        seg = RE_SEGMENT.match(line)
        if seg:
            segment = seg.group(1).strip()
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
            continue
        m = RE_MEMBER.match(line)
        if not m:
            continue
        name = m.group(1)
        raw_val = m.group(2)
        comment = (m.group(3) or "").strip()
        comment = re.sub(r"^<+\s*", "", comment)
        comment = re.sub(r"\s+", " ", comment)
        if raw_val:
            current = int(raw_val, 0)
        elif current is None:
            continue
        else:
            current += 1
        canonical = ""
        cm = RE_CANONICAL.search(comment)
        if cm:
            canonical = cm.group(1)
        payloads = RE_PAYLOAD_TOKEN.findall(comment)
        if "Empty" in comment and not payloads:
            payloads = ["Empty"]
        entries.append(MsgEntry(
            name=name,
            value=current,
            line=line_no,
            segment=segment,
            comment=comment,
            canonical=canonical,
            payload_names=payloads,
            suffix=_suffix(name),
        ))
    return entries


def _parse_modules(hpp: str) -> dict[str, tuple[int, str]]:
    m = RE_ENUM_MODULE.search(hpp)
    if not m:
        return {}
    out: dict[str, tuple[int, str]] = {}
    current = None
    for raw in m.group(1).splitlines():
        mem = RE_MEMBER.match(raw)
        if not mem:
            continue
        name = mem.group(1)
        raw_val = mem.group(2)
        comment = re.sub(r"\s+", " ", (mem.group(3) or "").strip())
        if raw_val:
            current = int(raw_val, 0)
        elif current is None:
            continue
        else:
            current += 1
        out[name] = (current, comment)
    return out


def _leading_comment_before(hpp: str, struct_start: int) -> str:
    before = hpp[:struct_start].rstrip()
    if not before.endswith("*/"):
        return ""
    idx = before.rfind("/**")
    if idx < 0:
        return ""
    block = before[idx:]
    if "struct " in block:
        return ""
    lines = []
    for ln in block.splitlines():
        ln = re.sub(r"^/\*\*|\*/$", "", ln)
        ln = re.sub(r"^\s*\*\s?", "", ln)
        if ln.strip():
            lines.append(ln.strip())
    return " ".join(lines)


def _parse_payloads(hpp: str) -> dict[str, PayloadStruct]:
    out: dict[str, PayloadStruct] = {}
    for m in RE_STRUCT.finditer(hpp):
        name = m.group(1)
        if not name.endswith("Payload") and name not in ("FsDirHeader", "WifiListHeader", "FileEntryPayload"):
            continue
        line = hpp.count("\n", 0, m.start()) + 1
        fields: list[tuple[str, str]] = []
        for fl in m.group(2).splitlines():
            fm = RE_FIELD.match(fl)
            if fm:
                typ = re.sub(r"\s+", " ", fm.group(1).strip())
                fname = fm.group(2) + (fm.group(3) or "")
                fields.append((typ, fname))
        out[name] = PayloadStruct(
            name=name,
            line=line,
            doc=_leading_comment_before(hpp, m.start()),
            fields=fields,
        )
    return out


def _parse_switch_names(hpp: str, regex: re.Pattern) -> set[str]:
    m = regex.search(hpp)
    if not m:
        return set()
    return set(RE_CASE_MSG.findall(m.group(1)))


def _parse_md_table(md: str) -> dict[str, dict]:
    """COMMON.md 消息总表：第一列可含多个 `MsgType`（用 / 连接）。"""
    rows: dict[str, dict] = {}
    in_table = False
    for line in md.splitlines():
        if line.startswith("| MsgType"):
            in_table = True
            continue
        if in_table and line.startswith("|---"):
            continue
        if in_table and not line.startswith("|"):
            break
        if not in_table:
            continue
        parts = [p.strip() for p in line.strip().strip("|").split("|")]
        if len(parts) < 6:
            continue
        enum_cell, canonical, direction, payload, ui_api, handler = parts[:6]
        names = RE_TICK.findall(enum_cell)
        if not names:
            continue
        rec = {
            "canonical": canonical.strip().strip("`") if canonical not in ("—", "-") else "",
            "direction": direction,
            "payload": payload,
            "ui_api": ui_api,
            "handler": handler,
            "md_cell": enum_cell,
        }
        for n in names:
            rows[n] = rec
    return rows


def _parse_alarms(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    rows: list[dict] = []
    with path.open(encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for raw in reader:
            code = (raw.get("code") or "").strip()
            if not code:
                continue
            try:
                code_i = int(code)
            except ValueError:
                continue
            rows.append({
                "index": (raw.get("index") or "").strip(),
                "macro": (raw.get("macro") or "").strip(),
                "name": (raw.get("name") or "").strip(),
                "code": code_i,
                "kind": (raw.get("kind") or "").strip(),
                "severity": (raw.get("severity") or "").strip(),
                "clear_mode": (raw.get("clear_mode") or "").strip(),
                "description": (raw.get("description") or "").strip(),
            })
    return rows


def alarm_snapshot_code_of(slot_id: int, bit: int) -> int:
    return 1000 + (slot_id // 3) * 100 + (slot_id % 3) * 32 + bit


def alarm_snapshot_slot_bit(code: int) -> Optional[tuple[int, int]]:
    if not (1000 <= code <= 1395 and ((code - 1000) % 100) < 96):
        return None
    off = code - 1000
    group = off // 100
    within = off % 100
    return group * 3 + within // 32, within % 32


def load_index(root: Path) -> ContractIndex:
    hpp_path = root / HPP_REL
    md_path = root / MD_REL
    csv_path = root / ALARM_CSV_REL
    hpp = hpp_path.read_text(encoding="utf-8")
    md = md_path.read_text(encoding="utf-8") if md_path.is_file() else ""

    mt = RE_ENUM_MSGTYPE.search(hpp)
    if not mt:
        raise RuntimeError(f"未找到 enum class MsgType: {hpp_path}")
    enum_line = hpp.count("\n", 0, mt.start()) + 1
    entries = _parse_enum_body(mt.group(1), enum_line)
    msgs = {e.name: e for e in entries}
    by_value = {e.value: e.name for e in entries}

    md_rows = _parse_md_table(md)
    for name, rec in md_rows.items():
        if name in msgs:
            msgs[name].md = rec

    return ContractIndex(
        root=root,
        hpp_mtime=hpp_path.stat().st_mtime,
        md_mtime=md_path.stat().st_mtime if md_path.is_file() else 0.0,
        csv_mtime=csv_path.stat().st_mtime if csv_path.is_file() else 0.0,
        msgs=msgs,
        by_value=by_value,
        modules=_parse_modules(hpp),
        payloads=_parse_payloads(hpp),
        reliable=_parse_switch_names(hpp, RE_RELIABLE_FN),
        latest_only=_parse_switch_names(hpp, RE_LATEST_FN),
        md_names=set(md_rows.keys()),
        alarms=_parse_alarms(csv_path),
    )


_CACHED: Optional[ContractIndex] = None


def get_index(root: Path) -> ContractIndex:
    global _CACHED
    hpp = root / HPP_REL
    md = root / MD_REL
    csvp = root / ALARM_CSV_REL
    mt = (
        hpp.stat().st_mtime,
        md.stat().st_mtime if md.is_file() else 0.0,
        csvp.stat().st_mtime if csvp.is_file() else 0.0,
    )
    if _CACHED is not None and (
        _CACHED.hpp_mtime,
        _CACHED.md_mtime,
        _CACHED.csv_mtime,
    ) == mt:
        return _CACHED
    _CACHED = load_index(root)
    return _CACHED


def guess_rsp_names(req_name: str, msgs: dict[str, MsgEntry]) -> list[str]:
    if req_name in SHARED_RSP:
        target = SHARED_RSP[req_name]
        return [target] if target in msgs else []
    if not req_name.endswith("_Req"):
        return []
    found: list[str] = []
    same = req_name[:-4] + "_Rsp"
    if same in msgs:
        found.append(same)
    for prefix, rsp_prefixes in REQ_PREFIX_TO_RSP:
        if not req_name.startswith(prefix):
            continue
        rest = req_name[len(prefix):]  # 含 _Req
        if not rest.endswith("_Req"):
            break
        action = rest[:-4]
        for rp in rsp_prefixes:
            cand = rp + action + "_Rsp"
            if cand in msgs and cand not in found:
                found.append(cand)
        break
    return found


def payload_for_msg(idx: ContractIndex, entry: MsgEntry) -> list[PayloadStruct]:
    out: list[PayloadStruct] = []
    names = list(entry.payload_names)
    if entry.md:
        names.extend(RE_PAYLOAD_TOKEN.findall(entry.md.get("payload") or ""))
    seen = set()
    for n in names:
        if n in seen or n == "Empty":
            continue
        seen.add(n)
        if n in idx.payloads:
            out.append(idx.payloads[n])
    return out

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MCU 报警字典生成器：CSV → app/ui/lib/core/src/ui_alarm_catalog_gen.cpp

用法（在仓库根目录）：
    python3 tools/gen_alarm_catalog.py

输入（均为 MCU 组随固件配套下发，勿手改）：
    doc/主控板串口通信协议 - 报警&事件表.csv     报警号字典（index,macro,name,code,kind,severity,clear_mode,description）
    doc/主控板串口通信协议 - 报警Details.csv    详情号字典（index,macro,code,reason,description）
    doc/主控板串口通信协议 - 报警快照Map.csv    快照位图槽位（index,word_index,bit_index,bit_mask_hex,macro,name,code）
输入（SOC 侧自己维护的英文翻译，按 code 覆盖 macro 自动转换的兜底文案）：
    doc/alarm_catalog_en.csv                    列：kind(alarm|detail),code,title_en,desc_en

输出（生成物，禁止手改；重新生成后 git diff 检查变化）：
    app/ui/lib/core/src/ui_alarm_catalog_gen.cpp

规则：
  - 显示名优先用翻译表 title_en；未翻译时由 macro 自动转成 sentence case
    （已知缩写保持原样，见 ACRONYMS；个别 macro 拼写错误在 MACRO_FIXES 修正）。
  - severity/clear_mode 进字典但**只作快照兜底**：快照（0xA3/0x81）只有 active/
    latched 位图，给不出这两个值，而它恰恰用在"本地表没有这条"的补齐场景。收到
    单条上报后必须以报文为准覆盖（见 SOC_TLV_ALARM_SINGLE_REPORT…§1）。
  - 快照 Map 只含 CONDITION，EVENT 不占 bit；两张表的 code 集合会交叉校验。
  - MCU 新增条目缺翻译、或翻译表出现孤儿 code 时在 stderr 提示，不阻塞生成。
"""

import csv
import datetime
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ALARM_CSV = os.path.join(ROOT, "doc", "主控板串口通信协议 - 报警&事件表.csv")
DETAIL_CSV = os.path.join(ROOT, "doc", "主控板串口通信协议 - 报警Details.csv")
SNAPSHOT_CSV = os.path.join(ROOT, "doc", "主控板串口通信协议 - 报警快照Map.csv")
EN_CSV = os.path.join(ROOT, "doc", "alarm_catalog_en.csv")
OUT_CPP = os.path.join(ROOT, "app", "ui", "lib", "core", "src",
                       "ui_alarm_catalog_gen.cpp")

# macro 分词后保持原样/特殊映射的词（其余单词转小写，句首再大写）
ACRONYMS = {
    "PD1": "PD1", "PD2": "PD2", "PD": "PD", "QCW": "QCW", "QBH": "QBH",
    "NFC": "NFC", "MCU": "MCU", "PCB": "PCB", "DB25": "DB25", "ALD": "ALD",
    "CH1": "CH1", "T1": "T1", "T2": "T2", "Z": "Z", "S": "S", "D": "D",
    "PM2": "PM2", "5": "5", "SG": "SG", "DIAG": "DIAG", "TMC": "TMC",
    "JOG": "JOG", "PWM": "PWM", "ESTOP": "E-stop",
}

# CSV 里 macro 本身的笔误修正（MCU 侧枚举不便改动，显示层兜住）
MACRO_FIXES = {
    "WSASTE_POWDER_TYPE_MISMATCH": "WASTE_POWDER_TYPE_MISMATCH",
}

DETAIL_MACRO_PREFIX = "ERROR_DETAILS_"

# 字典里的名字 → 报文数值。字典沿用 FAULT/TRIP 的老叫法，报文语义（information/
# warning/error/critical）见 doc/主控板串口通信协议 - 信息报警命令详表.csv，
# 数值和次序两边一致。
SEVERITY_VALUES = {"INFO": 1, "WARNING": 2, "FAULT": 3, "TRIP": 4}
CLEAR_MODE_VALUES = {"AUTO": 0, "RESET": 1, "POWER_CYCLE": 2, "NONE": 3}


def macro_to_text(macro: str) -> str:
    """SELF_TEST_FAILED → "Self test failed"；缩写保持原样。"""
    macro = MACRO_FIXES.get(macro, macro)
    words = []
    for i, tok in enumerate(macro.split("_")):
        if tok in ACRONYMS:
            words.append(ACRONYMS[tok])
        elif i == 0:
            words.append(tok.capitalize())
        else:
            words.append(tok.lower())
    text = " ".join(words)
    # PM2 5 → PM2.5（macro 里的 PM2_5 被下划线拆开）
    return text.replace("PM2 5", "PM2.5")


def reason_to_text(reason: str) -> str:
    """详情表 reason 列（safety_door_open）→ "Safety door open"。"""
    words = reason.strip().split("_")
    out = [words[0].capitalize()] + [w.lower() for w in words[1:]]
    text = " ".join(out)
    return text.replace("pm25", "PM2.5").replace("Pm25", "PM2.5")


def read_csv(path):
    with io.open(path, "r", encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def enum_value(table, raw, what, code):
    """字典里的枚举名 → 数值；未知值按最保守的一档处理并提示。"""
    key = (raw or "").strip().upper()
    if key in table:
        return table[key]
    print(f"warn: alarm {code} unknown {what} '{raw}', "
          f"falling back to safest value", file=sys.stderr)
    # severity 未知按 warning（不低于普通警告，见开发指导 §7）；
    # clear_mode 未知按 NONE（不显示复位钮，避免误发 RESET）。
    return 2 if what == "severity" else 3


def main() -> int:
    alarms = read_csv(ALARM_CSV)
    details = read_csv(DETAIL_CSV)
    snapshot = read_csv(SNAPSHOT_CSV)

    en_alarm, en_detail = {}, {}
    if os.path.exists(EN_CSV):
        for row in read_csv(EN_CSV):
            kind = (row.get("kind") or "").strip()
            code = (row.get("code") or "").strip()
            if not code or not kind:
                continue
            entry = ((row.get("title_en") or "").strip(),
                     (row.get("desc_en") or "").strip())
            if kind == "alarm":
                en_alarm[int(code)] = entry
            elif kind == "detail":
                en_detail[int(code)] = entry

    # 校验：code 重复 / 空 macro
    seen = set()
    for row in alarms:
        code = int(row["code"])
        if code in seen:
            print(f"ERROR: duplicate alarm code {code}", file=sys.stderr)
            return 1
        seen.add(code)
        if not (row.get("macro") or "").strip():
            print(f"ERROR: alarm {code} empty macro", file=sys.stderr)
            return 1

    # 提示：翻译缺失/孤儿（不阻塞）
    alarm_codes = {int(r["code"]) for r in alarms}
    detail_codes = {int(r["code"]) for r in details}
    missing_a = sorted(alarm_codes - set(en_alarm))
    missing_d = sorted(detail_codes - set(en_detail))
    if missing_a:
        print(f"note: {len(missing_a)} alarms use macro-derived text: "
              f"{missing_a}", file=sys.stderr)
    if missing_d:
        print(f"note: {len(missing_d)} details use reason-derived text: "
              f"{missing_d}", file=sys.stderr)
    for orphan in sorted(set(en_alarm) - alarm_codes):
        print(f"warn: en translation for removed alarm {orphan}", file=sys.stderr)
    for orphan in sorted(set(en_detail) - detail_codes):
        print(f"warn: en translation for removed detail {orphan}", file=sys.stderr)

    # 快照 Map 与报警表交叉校验：Map 只该含 CONDITION，且 code 必须在报警表里。
    # 对不上说明两份 CSV 不同步，快照对齐会把认不出的 bit 当成"未知报警"。
    event_codes = {int(r["code"]) for r in alarms
                   if (r.get("kind") or "").strip().upper() == "EVENT"}
    snap_codes = {int(r["code"]) for r in snapshot}
    for bad in sorted(snap_codes - alarm_codes):
        print(f"ERROR: snapshot map has alarm {bad} absent from alarm table",
              file=sys.stderr)
        return 1
    for bad in sorted(snap_codes & event_codes):
        print(f"ERROR: snapshot map contains EVENT {bad} (EVENT 不占 bit)",
              file=sys.stderr)
        return 1
    missing_slot = sorted(alarm_codes - event_codes - snap_codes)
    if missing_slot:
        print(f"warn: {len(missing_slot)} CONDITION without snapshot slot "
              f"(快照对齐不会管辖它们): {missing_slot}", file=sys.stderr)

    seen_slot = set()
    for row in snapshot:
        slot = (int(row["word_index"]), int(row["bit_index"]))
        if slot in seen_slot:
            print(f"ERROR: duplicate snapshot slot {slot}", file=sys.stderr)
            return 1
        seen_slot.add(slot)

    today = datetime.date.today().isoformat()
    lines = []
    lines.append("/* AUTO-GENERATED by tools/gen_alarm_catalog.py — DO NOT EDIT.")
    lines.append(" *")
    lines.append(f" * Generated {today} from:")
    lines.append(f" *   doc/主控板串口通信协议 - 报警&事件表.csv ({len(alarms)} alarms)")
    lines.append(f" *   doc/主控板串口通信协议 - 报警Details.csv ({len(details)} details)")
    lines.append(f" *   doc/主控板串口通信协议 - 报警快照Map.csv ({len(snapshot)} slots)")
    lines.append(" *   doc/alarm_catalog_en.csv (English overrides)")
    lines.append(" *")
    lines.append(" * 表里的 severity/clear_mode 只作快照兜底：单条上报到达时一律以报文为准。 */")
    lines.append("")
    lines.append('#include "ui_alarm_catalog.hpp"')
    lines.append("")
    lines.append("namespace {")
    lines.append("")

    lines.append("const ui_alarm_catalog_entry_t kAlarms[] = {")
    for row in alarms:
        code = int(row["code"])
        macro = row["macro"].strip()
        title, desc = en_alarm.get(code, ("", ""))
        if not title:
            title = macro_to_text(macro)
        sev = enum_value(SEVERITY_VALUES, row.get("severity"), "severity", code)
        clr = enum_value(CLEAR_MODE_VALUES, row.get("clear_mode"),
                         "clear_mode", code)
        is_event = 1 if (row.get("kind") or "").strip().upper() == "EVENT" else 0
        lines.append(f'    {{ {code}, "{c_escape(macro)}", '
                     f'"{c_escape(title)}", "{c_escape(desc)}", '
                     f'{sev}, {clr}, {is_event} }},')
    lines.append("};")
    lines.append("")

    lines.append("/* 快照位图槽位表：只含 CONDITION，按 (word,bit) 升序 */")
    lines.append("const ui_alarm_snapshot_entry_t kSnapshotSlots[] = {")
    for row in sorted(snapshot,
                      key=lambda r: (int(r["word_index"]), int(r["bit_index"]))):
        lines.append(f'    {{ {int(row["word_index"])}, '
                     f'{int(row["bit_index"])}, {int(row["code"])} }},')
    lines.append("};")
    lines.append("")

    lines.append("const ui_alarm_detail_entry_t kDetails[] = {")
    for row in details:
        code = int(row["code"])
        title, _ = en_detail.get(code, ("", ""))
        if not title:
            title = reason_to_text(row["reason"])
        lines.append(f'    {{ {code}u, "{c_escape(title)}" }},')
    lines.append("};")
    lines.append("")
    lines.append("} /* namespace */")
    lines.append("")
    lines.append(f'const char *const ui_alarm_catalog_version = "{today}";')
    lines.append("")
    lines.append("const ui_alarm_catalog_entry_t *ui_alarm_catalog_find(uint16_t code)")
    lines.append("{")
    lines.append("    for(const auto &e : kAlarms) {")
    lines.append("        if(e.code == code) {")
    lines.append("            return &e;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("const ui_alarm_detail_entry_t *ui_alarm_detail_find(uint32_t detail)")
    lines.append("{")
    lines.append("    for(const auto &e : kDetails) {")
    lines.append("        if(e.code == detail) {")
    lines.append("            return &e;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append("int ui_alarm_catalog_count(void)")
    lines.append("{")
    lines.append("    return (int)(sizeof(kAlarms) / sizeof(kAlarms[0]));")
    lines.append("}")
    lines.append("")
    lines.append("uint16_t ui_alarm_snapshot_code(uint8_t word_index, uint8_t bit_index)")
    lines.append("{")
    lines.append("    for(const auto &e : kSnapshotSlots) {")
    lines.append("        if(e.word_index == word_index && e.bit_index == bit_index) {")
    lines.append("            return e.code;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return 0;")
    lines.append("}")
    lines.append("")
    lines.append("bool ui_alarm_snapshot_slot_of(uint16_t code, uint8_t *word_index,")
    lines.append("                               uint8_t *bit_index)")
    lines.append("{")
    lines.append("    for(const auto &e : kSnapshotSlots) {")
    lines.append("        if(e.code != code) {")
    lines.append("            continue;")
    lines.append("        }")
    lines.append("        if(word_index) *word_index = e.word_index;")
    lines.append("        if(bit_index)  *bit_index  = e.bit_index;")
    lines.append("        return true;")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("int ui_alarm_snapshot_count(void)")
    lines.append("{")
    lines.append("    return (int)(sizeof(kSnapshotSlots) / sizeof(kSnapshotSlots[0]));")
    lines.append("}")
    lines.append("")

    with io.open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"wrote {os.path.relpath(OUT_CPP, ROOT)}: "
          f"{len(alarms)} alarms, {len(details)} details, "
          f"{len(snapshot)} snapshot slots")
    return 0


if __name__ == "__main__":
    sys.exit(main())

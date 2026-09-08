#!/usr/bin/env python3
"""Collect SnowX first-layer Visual logs and generate CSV/Markdown reports."""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - report generation remains usable without Pillow
    Image = ImageDraw = ImageFont = None


ROOT = Path(__file__).resolve().parent
DEFAULT_REPORT_ROOT = Path.home() / "snowx_first_layer_reports"
DEFAULT_SERIAL = "172.17.101.16:5555"
INFERENCE_MARKER = "first layer inference "
MODEL_MARKER = "first layer model "
SESSION_STOP_MARKER = "first layer session stop "
WINDOWS_CHINESE_FONT = Path(r"C:\Windows\Fonts\msyh.ttc")
TIMESTAMP_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)")
KEY_RE = re.compile(r"(?<!\S)([A-Za-z_][A-Za-z0-9_]*)=")

CSV_FIELDS = [
    "session_id",
    "timestamp",
    "status",
    "frame",
    "width",
    "height",
    "raw_class",
    "output_class",
    "ready_probability",
    "ready_threshold",
    "ready_margin",
    "confidence",
    "confidence_threshold",
    "ood",
    "ood_distance",
    "ood_threshold",
    "alignment",
    "cache",
    "cache_event",
    "segmentation_ran",
    "segmentation_ms",
    "preprocessing_ms",
    "fusion_ms",
    "total_ms",
    "rejection",
    "error",
    "result",
    "qualified_image_saved",
    "qualified_image_path",
    "qualified_image_local",
    "image_save_ms",
    "image_error",
    "source_file",
]

CSV_HEADERS_ZH = {
    "session_id": "会话编号",
    "timestamp": "时间",
    "status": "状态",
    "frame": "帧号",
    "width": "图像宽度",
    "height": "图像高度",
    "raw_class": "原始分类",
    "output_class": "输出分类",
    "ready_probability": "合格概率",
    "ready_threshold": "合格阈值",
    "ready_margin": "距阈值差值",
    "confidence": "置信度",
    "confidence_threshold": "置信度阈值",
    "ood": "分布外",
    "ood_distance": "分布外距离",
    "ood_threshold": "分布外阈值",
    "alignment": "几何对齐通过",
    "cache": "几何缓存可用",
    "cache_event": "缓存事件",
    "segmentation_ran": "本次运行分割",
    "segmentation_ms": "分割耗时ms",
    "preprocessing_ms": "预处理耗时ms",
    "fusion_ms": "融合分类耗时ms",
    "total_ms": "总耗时ms",
    "rejection": "拦截原因",
    "error": "错误信息",
    "result": "结果",
    "qualified_image_saved": "合格图已保存",
    "qualified_image_path": "合格图板端路径",
    "qualified_image_local": "合格图本地路径",
    "image_save_ms": "合格图保存耗时ms",
    "image_error": "合格图保存错误",
    "source_file": "日志文件",
}


def parse_key_values(text: str) -> dict[str, str]:
    matches = list(KEY_RE.finditer(text))
    values: dict[str, str] = {}
    for index, match in enumerate(matches):
        begin = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        values[match.group(1)] = text[begin:end].strip()
    return values


def parse_text(
    text: str, source_name: str
) -> tuple[list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    records: list[dict[str, str]] = []
    models: list[dict[str, str]] = []
    session_stops: list[dict[str, str]] = []
    for line in text.splitlines():
        timestamp_match = TIMESTAMP_RE.search(line)
        timestamp = timestamp_match.group(1) if timestamp_match else ""
        if INFERENCE_MARKER in line:
            fields = parse_key_values(line.split(INFERENCE_MARKER, 1)[1])
            fields["timestamp"] = timestamp
            fields["source_file"] = source_name
            if fields.get("ready_probability") and fields.get("ready_threshold"):
                try:
                    margin = float(fields["ready_probability"]) - float(fields["ready_threshold"])
                    fields["ready_margin"] = f"{margin:.6f}"
                except ValueError:
                    pass
            records.append(fields)
        elif MODEL_MARKER in line:
            fields = parse_key_values(line.split(MODEL_MARKER, 1)[1])
            fields["timestamp"] = timestamp
            fields["source_file"] = source_name
            models.append(fields)
        elif SESSION_STOP_MARKER in line:
            fields = parse_key_values(line.split(SESSION_STOP_MARKER, 1)[1])
            fields["timestamp"] = timestamp
            fields["source_file"] = source_name
            session_stops.append(fields)
    return records, models, session_stops


def parse_logs(
    paths: Iterable[Path],
) -> tuple[list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    records: list[dict[str, str]] = []
    models: list[dict[str, str]] = []
    session_stops: list[dict[str, str]] = []
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        parsed_records, parsed_models, parsed_stops = parse_text(text, path.name)
        records.extend(parsed_records)
        models.extend(parsed_models)
        session_stops.extend(parsed_stops)

    def deduplicate(items: list[dict[str, str]]) -> list[dict[str, str]]:
        unique: dict[str, dict[str, str]] = {}
        for item in items:
            key_data = {key: value for key, value in item.items() if key != "source_file"}
            key = json.dumps(key_data, sort_keys=True, ensure_ascii=False)
            unique[key] = item
        return sorted(unique.values(), key=lambda item: (item.get("timestamp", ""), item.get("frame", "")))

    return deduplicate(records), deduplicate(models), deduplicate(session_stops)


def select_sessions(
    records: list[dict[str, str]], session_stops: list[dict[str, str]]
) -> list[tuple[list[dict[str, str]], str, dict[str, str] | None]]:
    """Group every Visual print session; never concatenate records across sessions."""
    session_ids = {
        record.get("session_id", "")
        for record in records
        if record.get("session_id", "") not in {"", "0"}
    }
    if not session_ids:
        raise RuntimeError(
            "no session-aware inference records; deploy the session-report Visual update first"
        )

    def session_sort_key(value: str) -> tuple[int, str]:
        try:
            return int(value), value
        except ValueError:
            return -1, value

    sessions: list[tuple[list[dict[str, str]], str, dict[str, str] | None]] = []
    for session_id in sorted(session_ids, key=session_sort_key):
        selected = [record for record in records if record.get("session_id") == session_id]
        selected.sort(key=lambda record: (record.get("timestamp", ""), record.get("frame", "")))
        stops = [stop for stop in session_stops if stop.get("session_id") == session_id]
        latest_stop = max(stops, key=lambda stop: stop.get("timestamp", "")) if stops else None
        sessions.append((selected, session_id, latest_stop))
    return sessions


def select_latest_session(
    records: list[dict[str, str]], session_stops: list[dict[str, str]]
) -> tuple[list[dict[str, str]], str, dict[str, str] | None]:
    """Compatibility helper for tests and explicit --latest-only export."""
    sessions = select_sessions(records, session_stops)
    return sessions[-1]


def run_command(args: list[str], *, allow_failure: bool = False) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, text=True, capture_output=True, encoding="utf-8", errors="replace")
    if result.returncode != 0 and not allow_failure:
        command = " ".join(args)
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n{result.stderr.strip()}")
    return result


DIAGNOSTIC_ROOT_REMOTE = "/userdata/media/first_layer_visual"


def collect_from_board(adb: str, serial: str, output_dir: Path) -> list[Path]:
    raw_dir = output_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    if ":" in serial:
        connect = run_command([adb, "connect", serial], allow_failure=True)
        (raw_dir / "adb_connect.txt").write_text(
            connect.stdout + connect.stderr, encoding="utf-8"
        )
    run_command([adb, "-s", serial, "get-state"])

    logs: list[Path] = []
    for index in (1, 2, 3):
        remote = f"/var/log/rongsu_visual{index}.log"
        local = raw_dir / Path(remote).name
        pulled = run_command([adb, "-s", serial, "pull", remote, str(local)], allow_failure=True)
        if pulled.returncode == 0 and local.exists():
            logs.append(local)

    geometry = raw_dir / "first_layer_rknn_geometry.yml"
    run_command(
        [adb, "-s", serial, "pull", "/home/first_layer_rknn_geometry.yml", str(geometry)],
        allow_failure=True,
    )

    metadata_command = (
        "date; uname -a; "
        "sha256sum /home/rongsu_visual /home/rongsu_glavo "
        "/opt/first_layer_rknn/seg.rknn /opt/first_layer_rknn/fusion.rknn 2>/dev/null; "
        "ls -l /dev/dri/card1 /opt/first_layer_rknn/*.rknn 2>/dev/null"
    )
    metadata = run_command(
        [adb, "-s", serial, "shell", "sh", "-c", metadata_command], allow_failure=True
    )
    (raw_dir / "board_metadata.txt").write_text(
        metadata.stdout + metadata.stderr, encoding="utf-8"
    )
    if not logs:
        raise RuntimeError("no /var/log/rongsu_visual{1,2,3}.log files were collected")
    return logs


def referenced_diagnostic_paths(
    sessions: list[tuple[list[dict[str, str]], str, dict[str, str] | None]]
) -> list[str]:
    """Return only image files referenced by the sessions being exported."""
    paths: list[str] = []
    for records, _session_id, session_stop in sessions:
        for record in records:
            path = record.get("qualified_image_path", "")
            if path and path != "none":
                paths.append(path)
        if session_stop is not None:
            path = session_stop.get("rejected_image_path", "")
            if path and path != "none":
                paths.append(path)
    return list(dict.fromkeys(paths))


def collect_session_diagnostic_images(
    adb: str,
    serial: str,
    output_dir: Path,
    sessions: list[tuple[list[dict[str, str]], str, dict[str, str] | None]],
) -> Path:
    """Pull only diagnostic images referenced by selected report sessions.

    Pulling the whole board directory makes a one-session report download every
    historical image.  The logs already contain the exact saved-image paths.
    """
    diagnostic_images = output_dir / "raw" / "diagnostic_images"
    for remote_path in referenced_diagnostic_paths(sessions):
        if not remote_path.startswith(f"{DIAGNOSTIC_ROOT_REMOTE}/"):
            continue
        category = Path(remote_path).parent.name
        local_dir = diagnostic_images / category
        local_dir.mkdir(parents=True, exist_ok=True)
        run_command(
            [adb, "-s", serial, "pull", remote_path, str(local_dir / Path(remote_path).name)],
            allow_failure=True,
        )
    return diagnostic_images


def as_float(record: dict[str, str], key: str) -> float | None:
    try:
        return float(record[key])
    except (KeyError, TypeError, ValueError):
        return None


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def rejection_counter(records: list[dict[str, str]]) -> Counter[str]:
    counter: Counter[str] = Counter()
    for record in records:
        reasons = record.get("rejection", "")
        for reason in reasons.split(","):
            reason = reason.strip()
            if reason:
                counter[reason] += 1
    return counter


def would_pass_threshold(record: dict[str, str], threshold: float) -> bool:
    if record.get("status") != "ok" or record.get("raw_class") != "ready_for_first_layer":
        return False
    probability = as_float(record, "ready_probability")
    if probability is None or probability < threshold:
        return False
    fixed_rejections = {
        reason.strip()
        for reason in record.get("rejection", "").split(",")
        if reason.strip() and reason.strip() != "ready_probability_below_threshold"
    }
    return not fixed_rejections


def format_number(value: float | None, digits: int = 3) -> str:
    return "—" if value is None else f"{value:.{digits}f}"


def zh_cache_event(value: str) -> str:
    return {
        "cache_reused": "缓存复用",
        "cache_stable": "缓存稳定",
        "cache_updated": "缓存更新",
        "cache_created": "新建缓存",
        "not_run": "未执行",
    }.get(value, value or "未知")


def annotate_first_layer_image(
    image_path: Path, record: dict[str, str], powder_attempt: int
) -> None:
    """Translate report-copy overlays and mark the Nth first-layer powder spread."""
    if Image is None or not WINDOWS_CHINESE_FONT.exists():
        return
    try:
        with Image.open(image_path) as source:
            canvas = source.convert("RGB")
        draw = ImageDraw.Draw(canvas)
        title_font = ImageFont.truetype(str(WINDOWS_CHINESE_FONT), size=29)
        body_font = ImageFont.truetype(str(WINDOWS_CHINESE_FONT), size=23)
        badge_font = ImageFont.truetype(str(WINDOWS_CHINESE_FONT), size=30)

        # The board diagnostic has a 52 px English header and 88 px English footer.
        # Cover only those generated panels; the model input pixels remain untouched.
        draw.rectangle((0, 0, canvas.width, 52), fill=(24, 24, 24))
        draw.rectangle((0, canvas.height - 88, canvas.width, canvas.height), fill=(24, 24, 24))
        left_title = "用于检测的相机帧"
        right_title = "透视校正模型 ROI"
        draw.text((10, 10), left_title, font=title_font, fill=(240, 240, 240))
        title_box = draw.textbbox((0, 0), right_title, font=title_font)
        draw.text((canvas.width - (title_box[2] - title_box[0]) - 12, 10),
                  right_title, font=title_font, fill=(240, 240, 240))

        badge = f"第{powder_attempt}次铺粉检测"
        left, top, right, bottom = draw.textbbox((0, 0), badge, font=badge_font)
        text_width, text_height = right - left, bottom - top
        x, y = 16, 62
        padding_x, padding_y = 12, 8
        draw.rounded_rectangle(
            (x - padding_x, y - padding_y, x + text_width + padding_x, y + text_height + padding_y),
            radius=8,
            fill=(0, 0, 0),
            outline=(235, 235, 235),
            width=1,
        )
        draw.text((x, y), badge, font=badge_font, fill=(255, 255, 255))

        def value(key: str, fallback: str = "—") -> str:
            return record.get(key, "") or fallback

        first_line = (
            f"帧号={value('frame')}  合格概率={value('ready_probability')}  "
            f"阈值={value('ready_threshold')}  置信度={value('confidence')}  "
            f"总耗时={value('total_ms')}毫秒"
        )
        second_line = (
            f"缓存={zh_cache_event(value('cache_event', 'not_run'))}  "
            f"对齐={'通过' if value('alignment') == '1' else '未通过'}  "
            f"分割={'已执行' if value('segmentation_ran') == '1' else '未执行'}"
        )
        draw.text((10, canvas.height - 80), first_line, font=body_font, fill=(255, 255, 255))
        draw.text((10, canvas.height - 42), second_line, font=body_font, fill=(220, 220, 220))
        canvas.save(image_path, quality=92)
    except (OSError, ValueError):
        return


def write_report(
    output_dir: Path,
    records: list[dict[str, str]],
    models: list[dict[str, str]],
    source_description: str,
    session_id: str,
    session_stop: dict[str, str] | None,
    diagnostic_root: Path | None = None,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    def copy_session_image(
        board_path: str, target_dir_name: str, record: dict[str, str], powder_attempt: int
    ) -> str:
        if not board_path or board_path == "none" or diagnostic_root is None:
            return ""
        matches = list(diagnostic_root.rglob(Path(board_path).name))
        if not matches:
            return ""
        target_dir = output_dir / target_dir_name
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / matches[0].name
        shutil.copy2(matches[0], target)
        annotate_first_layer_image(target, record, powder_attempt)
        return target.relative_to(output_dir).as_posix()

    for powder_attempt, record in enumerate(records, start=1):
        board_path = record.get("qualified_image_path", "")
        record["qualified_image_local"] = copy_session_image(
            board_path, "qualified_images", record, powder_attempt
        )

    rejected_image_local = ""
    if session_stop is not None:
        rejected_image_local = copy_session_image(
            session_stop.get("rejected_image_path", ""), "rejected_images",
            records[-1] if records else {}, len(records)
        )

    csv_path = output_dir / "first_layer_inference.csv"
    with csv_path.open("w", encoding="utf-8-sig", newline="") as handle:
        chinese_fields = [CSV_HEADERS_ZH[field] for field in CSV_FIELDS]
        writer = csv.DictWriter(handle, fieldnames=chinese_fields, extrasaction="ignore")
        writer.writeheader()
        for record in records:
            writer.writerow({CSV_HEADERS_ZH[field]: record.get(field, "") for field in CSV_FIELDS})

    (output_dir / "model_contract_records.json").write_text(
        json.dumps(models, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    qualified_image_paths = [record["qualified_image_local"] for record in records
                             if record.get("qualified_image_local")]
    statuses = Counter(record.get("status", "unknown") for record in records)
    results = Counter(record.get("result", "unknown") for record in records)
    reasons = rejection_counter(records)
    ok_records = [record for record in records if record.get("status") == "ok"]

    latency_fields = [
        ("分割", "segmentation_ms"),
        ("预处理", "preprocessing_ms"),
        ("融合分类", "fusion_ms"),
        ("总耗时", "total_ms"),
    ]

    lines = [
        "# 首层视觉测试机推理报告",
        "",
        f"- 生成时间：{datetime.now().astimezone().isoformat(timespec='seconds')}",
        f"- 数据来源：{source_description}",
        f"- 本次打印会话：`{session_id}`",
        f"- 会话结束时间：{session_stop.get('timestamp', '仍在进行或未收到停止通知') if session_stop else '仍在进行或未收到停止通知'}",
        f"- 推理记录：{len(records)} 条",
        f"- 合格首层诊断图：{len(qualified_image_paths)} 张（本会话最多一张）",
        f"- 最终拦截诊断图：{1 if rejected_image_local else 0} 张（最终 result=0 时最多一张）",
        "- 说明：本报告反映模型输出和运行性能；没有人工真值时，不能计算准确率、召回率或误放率。",
        "",
        "## 结果汇总",
        "",
        "| 项目 | 数量 |",
        "|---|---:|",
        f"| 合格输出 result=1 | {results.get('1', 0)} |",
        f"| 保守拦截 result=0 | {results.get('0', 0)} |",
    ]
    for status, count in sorted(statuses.items()):
        lines.append(f"| 状态 `{status}` | {count} |")

    lines.extend(["", "## 推理耗时", "", "| 阶段 | 平均 ms | P50 ms | P95 ms |", "|---|---:|---:|---:|"])
    for label, field in latency_fields:
        values = [value for record in ok_records if (value := as_float(record, field)) is not None]
        mean = statistics.fmean(values) if values else None
        lines.append(
            f"| {label} | {format_number(mean)} | "
            f"{format_number(percentile(values, 0.50))} | {format_number(percentile(values, 0.95))} |"
        )

    lines.extend(["", "## 拦截原因", "", "| 原因 | 次数 |", "|---|---:|"])
    if reasons:
        for reason, count in reasons.most_common():
            lines.append(f"| `{reason}` | {count} |")
    else:
        lines.append("| 无 | 0 |")

    lines.extend(
        [
            "",
            "## 阈值回放（不重新运行 NPU）",
            "",
            "以下只改变合格概率阈值；低置信度、OOD、图像质量和定位拦截仍然保留。",
            "",
            "| 阈值 | 模拟合格 | 模拟拦截 |",
            "|---:|---:|---:|",
        ]
    )
    for step in range(50, 86, 5):
        threshold = step / 100.0
        passed = sum(would_pass_threshold(record, threshold) for record in records)
        lines.append(f"| {threshold:.2f} | {passed} | {len(records) - passed} |")

    if models:
        latest = models[-1]
        lines.extend(
            [
                "",
                "## 模型契约",
                "",
                f"- RKNN 契约：`{latest.get('rknn_contract', 'unknown')}`",
                f"- 分割模型 SHA256（期望）：`{latest.get('segmentation_sha256', 'unknown')}`",
                f"- 融合模型 SHA256（期望）：`{latest.get('fusion_sha256', 'unknown')}`",
                f"- 当前阈值：`{latest.get('ready_threshold', 'unknown')}`",
                f"- 内缩比例：`{latest.get('inset_scale', 'unknown')}`",
            ]
        )

    lines.extend(["", "## 合格首层诊断图", ""])
    linked_records = [
        record for record in records if record.get("qualified_image_local")
    ]
    if linked_records:
        lines.extend(
            [
                "左侧为完整相机帧，右侧为透视矫正后、进入光照预处理前的模型基板区域。",
                "",
                "| 时间 | 帧号 | 合格概率 | 图片 |",
                "|---|---:|---:|---|",
            ]
        )
        for record in linked_records:
            local = record["qualified_image_local"]
            lines.append(
                f"| {record.get('timestamp', '—')} | {record.get('frame', '—')} | "
                f"{record.get('ready_probability', '—')} | [{Path(local).name}]({local}) |"
            )
    else:
        lines.append("本次尚未拉取到合格首层诊断图。")

    lines.extend(["", "## 最终拦截诊断图", ""])
    if rejected_image_local:
        lines.extend(
            [
                "该图为会话结束时最近一次有效 `result=0` 的检测画面；不是每次拦截都存图。",
                "",
                f"[{Path(rejected_image_local).name}]({rejected_image_local})",
            ]
        )
    elif session_stop is not None and session_stop.get("final_result") == "0":
        lines.append("本会话最终为 result=0，但没有可保存的有效基板画面。")
    else:
        lines.append("本会话未产生最终 result=0 诊断图。")

    lines.extend(
        [
            "",
            "## 后续人工复核",
            "",
            "优先查看概率靠近当前阈值、`result=0` 且没有运行错误的样本；人工标注后再计算准确率、合格召回和不合格误放率。",
            "详细记录见 `first_layer_inference.csv`，板端原日志与模型哈希见 `raw/`。",
            "",
        ]
    )
    (output_dir / "first_layer_report.md").write_text("\n".join(lines), encoding="utf-8")


def write_batch_index(
    output_dir: Path,
    sessions: list[tuple[list[dict[str, str]], str, dict[str, str] | None]],
) -> None:
    lines = [
        "# 首层视觉批量报告索引",
        "",
        "本次导出按打印会话拆分，每个 `session_*` 目录只包含一次打印的 CSV、Markdown 和诊断图。",
        "",
        "| 打印会话 | 首次检测时间 | 检测次数 | 最终结果 | 报告 |",
        "|---|---|---:|---|---|",
    ]
    for records, session_id, session_stop in sessions:
        first_time = records[0].get("timestamp", "—") if records else "—"
        final_result = session_stop.get("final_result", "未结束") if session_stop else "未结束"
        relative_report = f"session_{session_id}/first_layer_report.md"
        lines.append(
            f"| `{session_id}` | {first_time} | {len(records)} | {final_result} | "
            f"[查看]({relative_report}) |"
        )
    (output_dir / "batch_index.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_self_test() -> None:
    sample = "\n".join(
        [
            "2026-08-18 09:59:01.100000 level=INFO proc=rongsu_visual src=x module=visual "
            "first layer inference report_v=1 session_id=1000 status=ok frame=9 width=1080 height=1280 "
            "raw_class=not_ready_for_first_layer output_class=not_ready_for_first_layer "
            "ready_probability=0.100000 ready_threshold=0.600000 confidence=0.900000 "
            "confidence_threshold=0.550000 ood=0 alignment=1 cache=1 result=0",
            "2026-08-18 10:00:00.100000 level=INFO proc=rongsu_visual src=x module=visual "
            "first layer model report_v=1 rknn_contract=test segmentation_sha256=seg "
            "fusion_sha256=fusion ready_threshold=0.600000 inset_scale=1.000000",
            "2026-08-18 10:00:01.100000 level=INFO proc=rongsu_visual src=x module=visual "
            "first layer inference report_v=1 session_id=1001 status=ok frame=10 width=1080 height=1280 "
            "raw_class=ready_for_first_layer output_class=ready_for_first_layer "
            "ready_probability=0.720000 ready_threshold=0.600000 confidence=0.720000 "
            "confidence_threshold=0.550000 ood=0 ood_distance=0.900000 ood_threshold=1.295949 "
            "alignment=1 cache=1 cache_event=cache_stable segmentation_ran=0 "
            "segmentation_ms=0.000 preprocessing_ms=4.000 fusion_ms=8.000 total_ms=12.000 "
            "rejection= result=1 qualified_image_saved=1 "
            "qualified_image_path=/userdata/media/first_layer_visual/qualified/test.jpg "
            "image_save_ms=18.000 image_error=none",
            "2026-08-18 10:00:02.100000 level=WARN proc=rongsu_visual src=x module=visual "
            "first layer inference report_v=1 session_id=1001 status=no_snapshot frame=0 width=0 height=0 "
            "result=0 rejection=no_snapshot",
            "2026-08-18 10:00:03.100000 level=INFO proc=rongsu_visual src=x module=visual "
            "first layer session stop session_id=1001 reason=first_layer_passed final_result=1 "
            "camera_stream=0 rejected_image_saved=0 rejected_image_path=none "
            "rejected_image_save_ms=0.000 rejected_image_error=none",
        ]
    )
    records, models, session_stops = parse_text(sample, "self_test.log")
    assert len(records) == 3 and len(models) == 1
    records, session_id, session_stop = select_latest_session(records, session_stops)
    assert session_id == "1001" and len(records) == 2 and session_stop is not None
    assert referenced_diagnostic_paths([(records, session_id, session_stop)]) == [
        "/userdata/media/first_layer_visual/qualified/test.jpg"
    ]
    assert records[0]["ready_margin"] == "0.120000"
    assert records[0]["qualified_image_saved"] == "1"
    assert would_pass_threshold(records[0], 0.70)
    assert not would_pass_threshold(records[0], 0.75)
    with tempfile.TemporaryDirectory() as temp:
        output = Path(temp)
        diagnostic_root = output / "raw_images"
        (diagnostic_root / "qualified").mkdir(parents=True)
        (diagnostic_root / "qualified" / "test.jpg").write_bytes(b"test")
        (diagnostic_root / "qualified" / "old.jpg").write_bytes(b"old")
        write_report(output, records, models, "self-test", session_id, session_stop, diagnostic_root)
        assert (output / "first_layer_inference.csv").exists()
        assert (output / "first_layer_report.md").exists()
        assert (output / "qualified_images" / "test.jpg").exists()
        assert not (output / "qualified_images" / "old.jpg").exists()
        assert "会话编号" in (output / "first_layer_inference.csv").read_text(encoding="utf-8-sig")
    print("self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default=DEFAULT_SERIAL, help="ADB serial, for example 172.17.101.16:5555")
    parser.add_argument("--adb", default=shutil.which("adb") or "adb", help="adb executable path")
    parser.add_argument("--output", type=Path, help="output directory")
    parser.add_argument("--log-dir", type=Path, help="parse existing local rongsu_visual*.log files")
    parser.add_argument(
        "--latest-only",
        action="store_true",
        help="only export the newest session; default exports every available session separately",
    )
    parser.add_argument(
        "--last-sessions",
        type=int,
        metavar="N",
        help="export the latest N sessions separately, for example --last-sessions 2",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return 0

    output = args.output or (
        DEFAULT_REPORT_ROOT / datetime.now().strftime("%Y%m%d_%H%M%S")
    )
    output = output.resolve()
    if args.log_dir:
        log_dir = args.log_dir.resolve()
        logs = sorted(log_dir.glob("rongsu_visual*.log"))
        if not logs:
            raise RuntimeError(f"no rongsu_visual*.log files in {log_dir}")
        source = str(log_dir)
    else:
        logs = collect_from_board(args.adb, args.serial, output)
        source = f"ADB {args.serial}"
    all_records, models, session_stops = parse_logs(logs)
    sessions = select_sessions(all_records, session_stops)
    if args.latest_only and args.last_sessions is not None:
        raise RuntimeError("--latest-only and --last-sessions cannot be used together")
    if args.last_sessions is not None:
        if args.last_sessions <= 0:
            raise RuntimeError("--last-sessions must be positive")
        sessions = sessions[-args.last_sessions:]
    elif args.latest_only:
        sessions = [sessions[-1]]

    diagnostic_root = (
        None
        if args.log_dir
        else collect_session_diagnostic_images(args.adb, args.serial, output, sessions)
    )

    exported_sessions: list[dict[str, object]] = []
    for records, session_id, session_stop in sessions:
        session_output = output / f"session_{session_id}"
        write_report(
            session_output, records, models, source, session_id, session_stop, diagnostic_root
        )
        exported_sessions.append(
            {
                "session_id": session_id,
                "records": len(records),
                "csv": str(session_output / "first_layer_inference.csv"),
                "report": str(session_output / "first_layer_report.md"),
            }
        )
    write_batch_index(output, sessions)
    print(json.dumps({
        "output": str(output),
        "sessions": exported_sessions,
        "index": str(output / "batch_index.md"),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)

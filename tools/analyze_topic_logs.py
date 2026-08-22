#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Analyze topic logs and generate troubleshooting reports.

Usage (run from repo root or any path):
    python tools/analyze_topic_logs.py --input "D:\\logs\\topic_dump"
    python tools/analyze_topic_logs.py --input ./var_log_topic --output ./analysis_out

Input:
    A directory that contains topic log files, such as:
      topic_boot_lifecycle.log
      topic_serial_sensor.log
      topic_serial_layer.log
      topic_print_flow.log
      topic_camera_capture.log
      topic_ui_preview.log
      topic_system_resource.log
      topic_mqtt_telemetry.log / topic_mqtt_command.log / topic_mqtt_wire.log / topic_mqtt_ota.log

Output:
    <output_dir>/
      summary.json
      key_events.csv
      metrics_env.csv
      metrics_laser.csv
      metrics_laser_setpoint.csv
      metrics_laser_power_compare.csv
      metrics_camera_capture.csv
      metrics_ui_preview.csv
      metrics_system_resource.csv
      metrics_mqtt.csv
      report.html
      plots/*.png              (only when matplotlib is available)
      env_interactive.html     (all environment metrics; interactive zoom/pan)

Notes:
    - Standard library only is enough for parsing + JSON/CSV/HTML output.
    - Optional plot dependency: matplotlib
        pip install matplotlib
    - Optional interactive dependency: plotly
        pip install plotly
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import glob
import json
import math
import os
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Tuple

try:
    import matplotlib.pyplot as plt  # type: ignore
except Exception:
    plt = None

try:
    import plotly.graph_objects as go  # type: ignore
    from plotly.subplots import make_subplots  # type: ignore
except Exception:
    go = None
    make_subplots = None


LOG_LINE_RE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}) "
    r"level=(?P<level>\w+) "
    r"proc=(?P<proc>\S+) "
    r"src=(?P<src>\S+) "
    r"module=(?P<module>\S+) "
    r"(?P<msg>.*)$"
)

KV_RE = re.compile(r"(?P<k>[A-Za-z_][A-Za-z0-9_\.]*)=(?P<v>[^\s]+)")

BOOT_RE = re.compile(
    r"PROCESS_BOOT proc=(?P<proc>\S+) pid=(?P<pid>-?\d+) "
    r"boot_id=(?P<boot_id>\S+) uptime_s=(?P<uptime_s>-?\d+) "
    r"prev_clean=(?P<prev_clean>-?\d+) prev_pid=(?P<prev_pid>-?\d+) prev_ts=(?P<prev_ts>-?\d+)"
)

CAPTURE_STREAM_RE = re.compile(
    r"stream 5s: committed=(?P<committed>\d+) last_seq=(?P<last_seq>\d+) "
    r"last_slot=(?P<last_slot>\d+) slot_fail=(?P<slot_fail>\d+) "
    r"dequeue_fail=(?P<dequeue_fail>\d+) release_timeout=(?P<release_timeout>\d+)"
)

UI_DECODE_RE = re.compile(
    r"decode 5s: .* ok=(?P<ok>\d+) fail=(?P<fail>\d+) no_frame=(?P<no_frame>\d+) .* "
    r"cvt_avg_us=(?P<cvt_avg_us>\d+) cvt_max_us=(?P<cvt_max_us>\d+) .* "
    r"loop_avg_us=(?P<loop_avg_us>\d+) loop_max_us=(?P<loop_max_us>\d+) rga=(?P<rga>\d+)"
)

SYS_RESOURCE_RE = re.compile(
    r"cpu_sys=(?P<cpu_sys>[0-9.]+)% cpu_proc=(?P<cpu_proc>[0-9.]+)% "
    r"temp=(?P<temp>[0-9.]+|N/A)C mem_avail=(?P<mem_avail>\d+)MB/(?P<mem_total>\d+)MB "
    r"rss=(?P<rss>\d+)MB"
)

MQTT_FRAME_RE = re.compile(
    r"MQTT (?P<dir>TX|RX) frame topic=(?P<topic>\S+) qos=(?P<qos>-?\d+) retained=(?P<retained>-?\d+) len=(?P<len>\d+)"
)

LASER_3ST_RE = re.compile(
    r"laser\(3ST\): type=(?P<type>\d+) status=0x(?P<status>[0-9A-Fa-f]+) "
    r"alarm=0x(?P<alarm>[0-9A-Fa-f]+) power=(?P<power>\d+) temp=(?P<temp>\d+) cur=(?P<cur>\d+)"
)
OVF_MARKING_POWER_RE = re.compile(
    r"marking params changed: key=(?P<key>-?\d+) power=(?P<power>\d+) feed=(?P<feed>[0-9.]+)"
)
SERIAL_TX_FRAME_RE = re.compile(r"SERIAL_TX frame\[(?P<len>\d+)\] (?P<body>[0-9A-Fa-f ]+)")
LASER_POWER_SET_RE = re.compile(r"laser_power_set raw=(?P<raw>\d+)")

# serial_debug 协议关键字段（激光手动功率）
SDBG_PORT_LASER = 0xAA
SDBG_CMD_LASER_MANUAL = 0x87
SDBG_TLV_LASER_PWR_SET = 0x01
SDBG_TLV_LASER_PWR_RESET = 0x02


def parse_hex_bytes(text: str) -> List[int]:
    return [int(h, 16) for h in re.findall(r"\b[0-9A-Fa-f]{2}\b", text)]


@dataclass
class LogRecord:
    ts: dt.datetime
    level: str
    proc: str
    src: str
    module: str
    msg: str
    file: str
    line_no: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze topic logs and generate summary, CSV metrics, and plots."
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Input folder containing topic_*.log files (can include subfolders).",
    )
    parser.add_argument(
        "--output",
        default="analysis_out",
        help="Output folder for reports. Default: ./analysis_out",
    )
    parser.add_argument(
        "--glob",
        default="**/topic*.log",
        help='Glob pattern under input dir. Default: "**/topic*.log"',
    )
    parser.add_argument(
        "--tz-offset",
        type=int,
        default=0,
        help="Optional hour offset applied when rendering report timestamps.",
    )
    parser.add_argument(
        "--no-plots",
        action="store_true",
        help="Disable plot generation even if matplotlib exists.",
    )
    parser.add_argument(
        "--no-interactive-env",
        action="store_true",
        help="Disable interactive environment chart even if plotly exists.",
    )
    return parser.parse_args()


def parse_ts(text: str) -> Optional[dt.datetime]:
    try:
        return dt.datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")
    except Exception:
        return None


def read_log_files(input_dir: str, pattern: str) -> List[str]:
    search = os.path.join(os.path.abspath(input_dir), pattern)
    files = sorted(glob.glob(search, recursive=True))
    return [f for f in files if os.path.isfile(f)]


def parse_log_line(line: str, file_path: str, line_no: int) -> Optional[LogRecord]:
    m = LOG_LINE_RE.match(line.rstrip("\n"))
    if not m:
        return None
    ts = parse_ts(m.group("ts"))
    if not ts:
        return None
    return LogRecord(
        ts=ts,
        level=m.group("level"),
        proc=m.group("proc"),
        src=m.group("src"),
        module=m.group("module"),
        msg=m.group("msg"),
        file=os.path.basename(file_path),
        line_no=line_no,
    )


def parse_value_num(raw: str) -> Optional[float]:
    s = raw.strip().rstrip(",")
    s = s.replace("%", "")
    s = s.replace("kPa", "")
    s = s.replace("C", "")
    if s.lower().startswith("0x"):
        try:
            return float(int(s, 16))
        except Exception:
            return None
    try:
        return float(s)
    except Exception:
        return None


def parse_kv_pairs(msg: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for m in KV_RE.finditer(msg):
        out[m.group("k")] = m.group("v")
    return out


def safe_mean(values: Iterable[float]) -> Optional[float]:
    arr = [v for v in values if v is not None and not math.isnan(v)]
    if not arr:
        return None
    return float(statistics.mean(arr))


def safe_max(values: Iterable[float]) -> Optional[float]:
    arr = [v for v in values if v is not None and not math.isnan(v)]
    if not arr:
        return None
    return float(max(arr))


def dt_to_str(ts: dt.datetime, tz_offset: int = 0) -> str:
    if tz_offset:
        ts = ts + dt.timedelta(hours=tz_offset)
    return ts.strftime("%Y-%m-%d %H:%M:%S.%f")


def write_csv(path: str, rows: List[Dict[str, Any]], headers: List[str]) -> None:
    with open(path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=headers)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def classify_restart_by_boot(boot_events: List[Dict[str, Any]]) -> Dict[str, Any]:
    if not boot_events:
        return {
            "boot_count": 0,
            "unique_process_count": 0,
            "system_reboot_count_estimate": 0,
            "process_restart_count": 0,
            "suspected_power_loss_boot_ids": [],
            "boot_sessions": [],
        }

    boot_events_sorted = sorted(boot_events, key=lambda x: x["ts"])
    boot_ids: List[str] = []
    by_boot: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for e in boot_events_sorted:
        bid = e["boot_id"]
        if bid not in by_boot:
            boot_ids.append(bid)
        by_boot[bid].append(e)

    process_restart_count = 0
    sessions: List[Dict[str, Any]] = []
    suspected_power_loss: List[str] = []
    all_processes = set()

    for bid in boot_ids:
        events = by_boot[bid]
        proc_counter = Counter(e["proc"] for e in events)
        repeated = {p: c for p, c in proc_counter.items() if c > 1}
        process_restart_count += sum(c - 1 for c in proc_counter.values() if c > 1)
        all_processes.update(proc_counter.keys())

        prev_clean_zero = sum(1 for e in events if e.get("prev_clean") == 0)
        if prev_clean_zero >= 2:
            suspected_power_loss.append(bid)

        sessions.append(
            {
                "boot_id": bid,
                "first_ts": dt_to_str(min(e["ts"] for e in events)),
                "last_ts": dt_to_str(max(e["ts"] for e in events)),
                "process_count": len(proc_counter),
                "events": len(events),
                "restarted_processes": repeated,
                "prev_clean_zero_count": prev_clean_zero,
                "uptime_s_min": min(e.get("uptime_s", -1) for e in events),
                "uptime_s_max": max(e.get("uptime_s", -1) for e in events),
            }
        )

    return {
        "boot_count": len(boot_events_sorted),
        "unique_process_count": len(all_processes),
        "system_reboot_count_estimate": max(0, len(boot_ids) - 1),
        "process_restart_count": process_restart_count,
        "suspected_power_loss_boot_ids": suspected_power_loss,
        "boot_sessions": sessions,
    }


def build_html_report(
    summary: Dict[str, Any],
    output_dir: str,
    generated_files: Dict[str, str],
) -> str:
    def html_escape(text: Any) -> str:
        s = str(text)
        return (
            s.replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace('"', "&quot;")
        )

    plots = generated_files.get("plots", [])
    env_interactive_html = generated_files.get("env_interactive_html", "")
    plot_html = ""
    for rel in plots:
        plot_html += f'<div class="card"><img src="{html_escape(rel)}" style="max-width:100%;height:auto;" /></div>\n'

    interactive_html_block = ""
    if env_interactive_html:
        interactive_html_block = (
            f'<div class="card"><p><a href="{html_escape(env_interactive_html)}" target="_blank">'
            "Open interactive environment chart (zoom/pan/detail)</a></p></div>"
        )

    top_alerts = summary.get("alerts", [])[:30]
    alert_rows = "".join(
        f"<tr><td>{html_escape(a.get('severity'))}</td><td>{html_escape(a.get('domain'))}</td><td>{html_escape(a.get('ts'))}</td><td>{html_escape(a.get('msg'))}</td></tr>"
        for a in top_alerts
    )

    boot_info = summary.get("boot", {})
    mqtt_info = summary.get("mqtt", {})
    camera_info = summary.get("camera", {})
    env_info = summary.get("environment", {})
    laser_info = summary.get("laser", {})

    html = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Topic Log Analysis Report</title>
  <style>
    body {{ font-family: Arial, sans-serif; margin: 20px; background: #fafafa; color: #222; }}
    h1, h2 {{ margin: 0.5em 0; }}
    .grid {{ display: grid; grid-template-columns: repeat(2, minmax(300px, 1fr)); gap: 12px; }}
    .card {{ background: #fff; border: 1px solid #ddd; border-radius: 8px; padding: 12px; }}
    table {{ border-collapse: collapse; width: 100%; background: #fff; }}
    th, td {{ border: 1px solid #ddd; padding: 6px; text-align: left; font-size: 13px; }}
    th {{ background: #f0f0f0; }}
    code {{ background: #f5f5f5; padding: 2px 4px; border-radius: 4px; }}
  </style>
</head>
<body>
  <h1>Topic Log Analysis Report</h1>
  <div class="grid">
    <div class="card">
      <h2>Boot</h2>
      <p>system_reboot_count_estimate: <b>{html_escape(boot_info.get("system_reboot_count_estimate", 0))}</b></p>
      <p>process_restart_count: <b>{html_escape(boot_info.get("process_restart_count", 0))}</b></p>
      <p>suspected_power_loss_boot_ids: <b>{html_escape(len(boot_info.get("suspected_power_loss_boot_ids", [])))}</b></p>
    </div>
    <div class="card">
      <h2>Camera</h2>
      <p>capture_samples: <b>{html_escape(camera_info.get("capture_samples", 0))}</b></p>
      <p>preview_samples: <b>{html_escape(camera_info.get("preview_samples", 0))}</b></p>
      <p>rga_soft_fallback_events: <b>{html_escape(camera_info.get("rga_soft_fallback_events", 0))}</b></p>
    </div>
    <div class="card">
      <h2>Environment</h2>
      <p>sensor_samples: <b>{html_escape(env_info.get("sensor_samples", 0))}</b></p>
      <p>env_sim_samples: <b>{html_escape(env_info.get("env_sim_samples", 0))}</b></p>
      <p>powder_low_events: <b>{html_escape(env_info.get("powder_low_events", 0))}</b></p>
    </div>
    <div class="card">
      <h2>MQTT</h2>
      <p>connect_ok: <b>{html_escape(mqtt_info.get("connect_ok", 0))}</b></p>
      <p>publish_fail: <b>{html_escape(mqtt_info.get("publish_fail", 0))}</b></p>
      <p>tx_frames: <b>{html_escape(mqtt_info.get("tx_frames", 0))}</b></p>
    </div>
    <div class="card">
      <h2>Laser (3ST)</h2>
      <p>samples: <b>{html_escape(laser_info.get("samples", 0))}</b></p>
      <p>setpoint_samples: <b>{html_escape(laser_info.get("setpoint_samples", 0))}</b></p>
      <p>compare_samples: <b>{html_escape(laser_info.get("compare_samples", 0))}</b></p>
      <p>alarm_events: <b>{html_escape(laser_info.get("alarm_events", 0))}</b></p>
      <p>setpoint_raw_avg: <b>{html_escape(laser_info.get("setpoint_raw_avg", None))}</b></p>
      <p>report_power_raw_avg: <b>{html_escape(laser_info.get("report_power_raw_avg", None))}</b></p>
      <p>power_raw_abs_err_avg: <b>{html_escape(laser_info.get("power_raw_abs_err_avg", None))}</b></p>
    </div>
  </div>

  <h2>Top Alerts</h2>
  <table>
    <thead><tr><th>Severity</th><th>Domain</th><th>Timestamp</th><th>Message</th></tr></thead>
    <tbody>{alert_rows}</tbody>
  </table>

  <h2>Plots</h2>
  {interactive_html_block}
  {plot_html}

  <h2>Generated Files</h2>
  <ul>
    <li><code>{html_escape(generated_files.get("summary_json", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("key_events_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_env_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_laser_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_laser_setpoint_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_laser_power_compare_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_camera_capture_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_ui_preview_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_system_resource_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("metrics_mqtt_csv", ""))}</code></li>
    <li><code>{html_escape(generated_files.get("env_interactive_html", ""))}</code></li>
  </ul>
</body>
</html>
"""
    path = os.path.join(output_dir, "report.html")
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)
    return path


def extract_from_logs(
    records: List[LogRecord],
    tz_offset: int = 0,
) -> Tuple[Dict[str, Any], Dict[str, List[Dict[str, Any]]]]:
    data: Dict[str, List[Dict[str, Any]]] = {
        "boot_events": [],
        "key_events": [],
        "metrics_env": [],
        "metrics_laser": [],
        "metrics_laser_setpoint": [],
        "metrics_laser_power_compare": [],
        "metrics_camera_capture": [],
        "metrics_ui_preview": [],
        "metrics_system_resource": [],
        "metrics_mqtt": [],
    }

    mqtt_stats = Counter()
    layer_cmd_stats = Counter()
    laser_set_events: List[Dict[str, Any]] = []

    for r in records:
        msg = r.msg
        ts_str = dt_to_str(r.ts, tz_offset=tz_offset)

        # Boot lifecycle
        bm = BOOT_RE.search(msg)
        if bm:
            b = {
                "ts": r.ts,
                "ts_text": ts_str,
                "proc": bm.group("proc"),
                "pid": int(bm.group("pid")),
                "boot_id": bm.group("boot_id"),
                "uptime_s": int(bm.group("uptime_s")),
                "prev_clean": int(bm.group("prev_clean")),
                "prev_pid": int(bm.group("prev_pid")),
                "prev_ts": int(bm.group("prev_ts")),
                "file": r.file,
                "line_no": r.line_no,
            }
            data["boot_events"].append(b)
            if b["prev_clean"] == 0:
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "BOOT",
                        "ts": ts_str,
                        "msg": f"PROCESS_BOOT prev_clean=0 proc={b['proc']} boot_id={b['boot_id']}",
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            continue

        # Environment sensor / env_sim
        if r.module == "McuLink" and (msg.startswith("sensor:") or msg.startswith("env_sim:")):
            pairs = parse_kv_pairs(msg)
            row: Dict[str, Any] = {
                "ts": ts_str,
                "proc": r.proc,
                "kind": "env_sim" if msg.startswith("env_sim:") else "sensor",
                "O2": parse_value_num(pairs.get("O2", "nan")) if "O2" in pairs else None,
                "P1_kPa": parse_value_num(pairs.get("P1", "nan")) if "P1" in pairs else None,
                "T_C": parse_value_num(pairs.get("T", "nan")) if "T" in pairs else None,
                "RH_pct": parse_value_num(pairs.get("RH", "nan")) if "RH" in pairs else None,
                "PM1_0": parse_value_num(pairs.get("PM1.0", "nan")) if "PM1.0" in pairs else None,
                "PM2_5": parse_value_num(pairs.get("PM2.5", "nan")) if "PM2.5" in pairs else None,
                "PM10": parse_value_num(pairs.get("PM10", "nan")) if "PM10" in pairs else None,
                "Pfilter_kPa": parse_value_num(pairs.get("Pfilter", "nan")) if "Pfilter" in pairs else None,
                "P2_kPa": parse_value_num(pairs.get("P2", "nan")) if "P2" in pairs else None,
                "base_C": parse_value_num(pairs.get("base", "nan")) if "base" in pairs else None,
                "powder_bits": int(pairs.get("powder_bits", "0"), 16)
                if pairs.get("powder_bits", "").lower().startswith("0x")
                else None,
                "powder_ok": parse_value_num(pairs.get("powder_ok", "nan")) if "powder_ok" in pairs else None,
                "door_open": parse_value_num(pairs.get("door_open", pairs.get("door", "nan")))
                if ("door_open" in pairs or "door" in pairs)
                else None,
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_env"].append(row)
            if row.get("powder_bits") == 0:
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "ENV",
                        "ts": ts_str,
                        "msg": "powder_bits=0x00 (powder low)",
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            continue

        # Laser 3ST report
        lm = LASER_3ST_RE.search(msg)
        if lm:
            laser_type = int(lm.group("type"))
            status = int(lm.group("status"), 16)
            alarm = int(lm.group("alarm"), 16)
            power_raw = int(lm.group("power"))
            temp_raw = int(lm.group("temp"))
            current_raw = int(lm.group("cur"))

            # 估算展示值（与 UI 口径一致，便于排查）
            if laser_type == 2:  # AL 200W
                power_pct_est = power_raw / 10.0
                temp_c_est = temp_raw / 10.0
                current_a_est = current_raw / 10.0
            else:  # JFSC 或未知默认按 JFSC 口径
                power_pct_est = (power_raw * 100.0 / 255.0) if power_raw <= 255 else None
                temp_c_est = float(temp_raw)
                current_a_est = current_raw / 100.0

            row = {
                "ts": ts_str,
                "proc": r.proc,
                "laser_type": laser_type,
                "status": status,
                "alarm": alarm,
                "power_raw": power_raw,
                "temp_raw": temp_raw,
                "current_raw": current_raw,
                "power_pct_est": power_pct_est,
                "temp_c_est": temp_c_est,
                "current_a_est": current_a_est,
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_laser"].append(row)

            if alarm != 0:
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "LASER",
                        "ts": ts_str,
                        "msg": (
                            f"laser alarm active type={laser_type} "
                            f"status=0x{status:04X} alarm=0x{alarm:04X}"
                        ),
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            continue

        # 统一功率包下发日志（McuLink 实际发送链路，已做“仅变化打印”）
        pm = LASER_POWER_SET_RE.search(msg)
        if r.module == "McuLink" and pm:
            set_raw = int(pm.group("raw"))
            row = {
                "ts": ts_str,
                "proc": r.proc,
                "source": "power_packet",
                "setpoint_raw": set_raw,
                "feed_mm_min": None,
                "key": None,
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_laser_setpoint"].append(row)
            laser_set_events.append(
                {
                    "ts": r.ts,
                    "source": row["source"],
                    "setpoint_raw": set_raw,
                }
            )
            continue

        # OVF 过程中的功率设定（推入 TCP 功率包前的设定值）
        om = OVF_MARKING_POWER_RE.search(msg)
        if om:
            set_raw = int(om.group("power"))
            row = {
                "ts": ts_str,
                "proc": r.proc,
                "source": "ovf_marking_params",
                "setpoint_raw": set_raw,
                "feed_mm_min": float(om.group("feed")),
                "key": int(om.group("key")),
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_laser_setpoint"].append(row)
            laser_set_events.append(
                {
                    "ts": r.ts,
                    "source": row["source"],
                    "setpoint_raw": set_raw,
                }
            )
            continue

        # 调试页激光手动功率设定（从串口 TX 十六进制帧反解）
        if r.module == "McuLink":
            sm = SERIAL_TX_FRAME_RE.search(msg)
            if sm:
                frame = parse_hex_bytes(sm.group("body"))
                if len(frame) >= 10 and frame[0] == 0x5A and frame[1] == 0xA5 and frame[2] == 0xAA:
                    port = frame[3]
                    cmd = frame[4]
                    payload_len = frame[6]
                    tlv_type = frame[7]
                    tlv_len = frame[8]
                    if (
                        port == SDBG_PORT_LASER
                        and cmd == SDBG_CMD_LASER_MANUAL
                        and tlv_type == SDBG_TLV_LASER_PWR_SET
                        and tlv_len == 1
                        and len(frame) >= 10
                    ):
                        percent = int(frame[9])
                        set_raw = int(round(percent * 255.0 / 100.0))
                        row = {
                            "ts": ts_str,
                            "proc": r.proc,
                            "source": "serial_debug_laser_power_set",
                            "setpoint_raw": set_raw,
                            "feed_mm_min": None,
                            "key": None,
                            "file": r.file,
                            "line_no": r.line_no,
                        }
                        data["metrics_laser_setpoint"].append(row)
                        laser_set_events.append(
                            {
                                "ts": r.ts,
                                "source": row["source"],
                                "setpoint_raw": set_raw,
                            }
                        )
                        continue
                    if (
                        port == SDBG_PORT_LASER
                        and cmd == SDBG_CMD_LASER_MANUAL
                        and tlv_type == SDBG_TLV_LASER_PWR_RESET
                    ):
                        row = {
                            "ts": ts_str,
                            "proc": r.proc,
                            "source": "serial_debug_laser_power_reset",
                            "setpoint_raw": None,
                            "feed_mm_min": None,
                            "key": None,
                            "file": r.file,
                            "line_no": r.line_no,
                        }
                        data["metrics_laser_setpoint"].append(row)
                        laser_set_events.append(
                            {
                                "ts": r.ts,
                                "source": row["source"],
                                "setpoint_raw": None,
                            }
                        )
                        continue

        # Layer / print-flow logic
        if "serial TX cmd:" in msg:
            cmd = msg.split("serial TX cmd:", 1)[1].strip()
            layer_cmd_stats[cmd] += 1
            data["key_events"].append(
                {
                    "severity": "INFO",
                    "domain": "LAYER",
                    "ts": ts_str,
                    "msg": f"serial cmd {cmd}",
                    "file": r.file,
                    "line_no": r.line_no,
                }
            )
        if "first layer visual check failed" in msg.lower() or "recoat" in msg.lower():
            data["key_events"].append(
                {
                    "severity": "WARN",
                    "domain": "LAYER",
                    "ts": ts_str,
                    "msg": msg,
                    "file": r.file,
                    "line_no": r.line_no,
                }
            )
        if "print_pause accepted" in msg or "user dispatch paused=1" in msg:
            data["key_events"].append(
                {
                    "severity": "INFO",
                    "domain": "PRINT_FLOW",
                    "ts": ts_str,
                    "msg": msg,
                    "file": r.file,
                    "line_no": r.line_no,
                }
            )

        # Camera capture stream stats (SHM producer side)
        cm = CAPTURE_STREAM_RE.search(msg)
        if cm:
            row = {
                "ts": ts_str,
                "proc": r.proc,
                "committed": int(cm.group("committed")),
                "last_seq": int(cm.group("last_seq")),
                "last_slot": int(cm.group("last_slot")),
                "slot_fail": int(cm.group("slot_fail")),
                "dequeue_fail": int(cm.group("dequeue_fail")),
                "release_timeout": int(cm.group("release_timeout")),
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_camera_capture"].append(row)
            if row["slot_fail"] > 0 or row["dequeue_fail"] > 0 or row["release_timeout"] > 0:
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "CAMERA_CAPTURE",
                        "ts": ts_str,
                        "msg": (
                            f"capture anomalies slot_fail={row['slot_fail']} "
                            f"dequeue_fail={row['dequeue_fail']} release_timeout={row['release_timeout']}"
                        ),
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            continue

        # UI preview decode stats
        um = UI_DECODE_RE.search(msg)
        if um:
            row = {
                "ts": ts_str,
                "proc": r.proc,
                "ok": int(um.group("ok")),
                "fail": int(um.group("fail")),
                "no_frame": int(um.group("no_frame")),
                "cvt_avg_us": int(um.group("cvt_avg_us")),
                "cvt_max_us": int(um.group("cvt_max_us")),
                "loop_avg_us": int(um.group("loop_avg_us")),
                "loop_max_us": int(um.group("loop_max_us")),
                "rga": int(um.group("rga")),
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_ui_preview"].append(row)
            if row["fail"] > 0 or row["no_frame"] > 0:
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "UI_PREVIEW",
                        "ts": ts_str,
                        "msg": f"decode anomalies fail={row['fail']} no_frame={row['no_frame']}",
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            if row["rga"] == 0:
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "UI_PREVIEW",
                        "ts": ts_str,
                        "msg": "RGA path disabled or fallback to software",
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            continue

        if "RGA streaming cache prepare failed" in msg or "fallback to software convert" in msg:
            data["key_events"].append(
                {
                    "severity": "WARN",
                    "domain": "UI_PREVIEW",
                    "ts": ts_str,
                    "msg": msg,
                    "file": r.file,
                    "line_no": r.line_no,
                }
            )

        # SHM ring related events
        if "shm" in msg.lower() or "ring opened" in msg.lower() or "peer shm" in msg.lower():
            sev = "INFO"
            if "failed" in msg.lower() or "fail" in msg.lower():
                sev = "WARN"
            data["key_events"].append(
                {
                    "severity": sev,
                    "domain": "SHM",
                    "ts": ts_str,
                    "msg": msg,
                    "file": r.file,
                    "line_no": r.line_no,
                }
            )

        # System resource
        sm = SYS_RESOURCE_RE.search(msg)
        if sm:
            temp = None if sm.group("temp") == "N/A" else float(sm.group("temp"))
            row = {
                "ts": ts_str,
                "proc": r.proc,
                "cpu_sys_pct": float(sm.group("cpu_sys")),
                "cpu_proc_pct": float(sm.group("cpu_proc")),
                "temp_c": temp,
                "mem_avail_mb": int(sm.group("mem_avail")),
                "mem_total_mb": int(sm.group("mem_total")),
                "rss_mb": int(sm.group("rss")),
                "file": r.file,
                "line_no": r.line_no,
            }
            data["metrics_system_resource"].append(row)
            continue

        # MQTT
        if r.module == "MqttService" or "MQTT " in msg:
            mqtt_row = {
                "ts": ts_str,
                "level": r.level,
                "msg": msg,
                "file": r.file,
                "line_no": r.line_no,
                "event": "",
                "topic": "",
                "direction": "",
                "payload_len": "",
                "qos": "",
                "retained": "",
            }
            mm = MQTT_FRAME_RE.search(msg)
            if mm:
                mqtt_row["event"] = "frame"
                mqtt_row["topic"] = mm.group("topic")
                mqtt_row["direction"] = mm.group("dir")
                mqtt_row["payload_len"] = int(mm.group("len"))
                mqtt_row["qos"] = int(mm.group("qos"))
                mqtt_row["retained"] = int(mm.group("retained"))
                if mm.group("dir") == "TX":
                    mqtt_stats["tx_frames"] += 1
                else:
                    mqtt_stats["rx_frames"] += 1
            elif "MQTT connected" in msg:
                mqtt_row["event"] = "connect_ok"
                mqtt_stats["connect_ok"] += 1
            elif "MQTT connect failed" in msg:
                mqtt_row["event"] = "connect_fail"
                mqtt_stats["connect_fail"] += 1
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "MQTT",
                        "ts": ts_str,
                        "msg": msg,
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            elif "publish telemetry fail" in msg:
                mqtt_row["event"] = "telemetry_fail"
                mqtt_stats["publish_fail"] += 1
                mqtt_stats["telemetry_fail"] += 1
            elif "publish heartbeat fail" in msg:
                mqtt_row["event"] = "heartbeat_fail"
                mqtt_stats["publish_fail"] += 1
                mqtt_stats["heartbeat_fail"] += 1
            elif "publish cmd/ack fail" in msg:
                mqtt_row["event"] = "cmd_ack_fail"
                mqtt_stats["publish_fail"] += 1
                mqtt_stats["cmd_ack_fail"] += 1
            elif "MQTT TX telemetry queued" in msg:
                mqtt_row["event"] = "telemetry_queued"
                mqtt_stats["telemetry_queued"] += 1
            elif "MQTT TX heartbeat queued" in msg:
                mqtt_row["event"] = "heartbeat_queued"
                mqtt_stats["heartbeat_queued"] += 1
            elif "MQTT TX cmd/ack queued" in msg:
                mqtt_row["event"] = "cmd_ack_queued"
                mqtt_stats["cmd_ack_queued"] += 1
            elif "MQTT subscribed topic=" in msg:
                mqtt_row["event"] = "subscribed"
                mqtt_stats["subscribed"] += 1
            elif "device activation failed" in msg:
                mqtt_row["event"] = "activation_fail"
                mqtt_stats["activation_fail"] += 1
                data["key_events"].append(
                    {
                        "severity": "WARN",
                        "domain": "MQTT",
                        "ts": ts_str,
                        "msg": msg,
                        "file": r.file,
                        "line_no": r.line_no,
                    }
                )
            elif "device activation succeeded" in msg:
                mqtt_row["event"] = "activation_ok"
                mqtt_stats["activation_ok"] += 1

            data["metrics_mqtt"].append(mqtt_row)

    # Build summary
    boot_summary = classify_restart_by_boot(data["boot_events"])

    env_rows = data["metrics_env"]
    sensor_rows = [r for r in env_rows if r.get("kind") == "sensor"]
    env_sim_rows = [r for r in env_rows if r.get("kind") == "env_sim"]
    powder_low_events = sum(1 for r in env_rows if r.get("powder_bits") == 0)

    cap_rows = data["metrics_camera_capture"]
    ui_rows = data["metrics_ui_preview"]
    sys_rows = data["metrics_system_resource"]
    laser_rows = data["metrics_laser"]
    setpoint_rows = data["metrics_laser_setpoint"]
    laser_type_counts = Counter(str(r.get("laser_type", 0)) for r in laser_rows)

    # 将“设定功率”对齐到每条激光上报，方便时序比较（取最近一次设定）
    laser_set_events.sort(key=lambda x: x["ts"])
    laser_reports_sorted = sorted(
        laser_rows,
        key=lambda x: parse_ts_text(str(x.get("ts", ""))) or dt.datetime.min
    )
    set_idx = 0
    latest_set_raw = None
    latest_set_src = ""
    for lr in laser_reports_sorted:
        t = parse_ts_text(str(lr.get("ts", "")))
        if not t:
            continue
        while set_idx < len(laser_set_events) and laser_set_events[set_idx]["ts"] <= t:
            latest_set_raw = laser_set_events[set_idx].get("setpoint_raw")
            latest_set_src = str(laser_set_events[set_idx].get("source", ""))
            set_idx += 1
        report_raw = lr.get("power_raw")
        delta_raw = None
        abs_err_raw = None
        if latest_set_raw is not None and report_raw is not None:
            delta_raw = int(report_raw) - int(latest_set_raw)
            abs_err_raw = abs(delta_raw)
        data["metrics_laser_power_compare"].append(
            {
                "ts": lr.get("ts"),
                "proc": lr.get("proc"),
                "laser_type": lr.get("laser_type"),
                "setpoint_source": latest_set_src if latest_set_src else None,
                "setpoint_raw": latest_set_raw,
                "report_power_raw": lr.get("power_raw"),
                "delta_raw": delta_raw,
                "abs_err_raw": abs_err_raw,
                "report_status": lr.get("status"),
                "report_alarm": lr.get("alarm"),
                "file": lr.get("file"),
                "line_no": lr.get("line_no"),
            }
        )
    compare_rows = data["metrics_laser_power_compare"]

    rga_soft_fallback_events = sum(
        1
        for e in data["key_events"]
        if e["domain"] == "UI_PREVIEW" and "software" in e["msg"].lower()
    )

    summary = {
        "boot": boot_summary,
        "environment": {
            "sensor_samples": len(sensor_rows),
            "env_sim_samples": len(env_sim_rows),
            "powder_low_events": powder_low_events,
            "pm25_avg": safe_mean([r.get("PM2_5") for r in sensor_rows]),
            "pm10_avg": safe_mean([r.get("PM10") for r in sensor_rows]),
            "temp_avg_c": safe_mean([r.get("T_C") for r in sensor_rows]),
            "humidity_avg_pct": safe_mean([r.get("RH_pct") for r in sensor_rows]),
        },
        "camera": {
            "capture_samples": len(cap_rows),
            "preview_samples": len(ui_rows),
            "capture_slot_fail_total": int(sum(r["slot_fail"] for r in cap_rows)) if cap_rows else 0,
            "capture_dequeue_fail_total": int(sum(r["dequeue_fail"] for r in cap_rows)) if cap_rows else 0,
            "capture_release_timeout_total": int(sum(r["release_timeout"] for r in cap_rows)) if cap_rows else 0,
            "preview_fail_total": int(sum(r["fail"] for r in ui_rows)) if ui_rows else 0,
            "preview_no_frame_total": int(sum(r["no_frame"] for r in ui_rows)) if ui_rows else 0,
            "preview_cvt_avg_us_mean": safe_mean([float(r["cvt_avg_us"]) for r in ui_rows]),
            "preview_cvt_max_us_max": safe_max([float(r["cvt_max_us"]) for r in ui_rows]),
            "rga_soft_fallback_events": rga_soft_fallback_events,
        },
        "laser": {
            "samples": len(laser_rows),
            "setpoint_samples": len(setpoint_rows),
            "compare_samples": len(compare_rows),
            "alarm_events": sum(1 for r in laser_rows if int(r.get("alarm", 0)) != 0),
            "type_counts": dict(laser_type_counts),
            "setpoint_raw_avg": safe_mean([
                float(r["setpoint_raw"]) for r in setpoint_rows
                if r.get("setpoint_raw") is not None
            ]),
            "report_power_raw_avg": safe_mean([
                float(r["power_raw"]) for r in laser_rows
                if r.get("power_raw") is not None
            ]),
            "power_raw_max": safe_max([float(r["power_raw"]) for r in laser_rows]),
            "temp_raw_max": safe_max([float(r["temp_raw"]) for r in laser_rows]),
            "current_raw_max": safe_max([float(r["current_raw"]) for r in laser_rows]),
            "power_pct_est_avg": safe_mean([
                float(r["power_pct_est"]) for r in laser_rows
                if r.get("power_pct_est") is not None
            ]),
            "temp_c_est_max": safe_max([
                float(r["temp_c_est"]) for r in laser_rows
                if r.get("temp_c_est") is not None
            ]),
            "current_a_est_max": safe_max([
                float(r["current_a_est"]) for r in laser_rows
                if r.get("current_a_est") is not None
            ]),
            "power_raw_abs_err_avg": safe_mean([
                float(r["abs_err_raw"]) for r in compare_rows
                if r.get("abs_err_raw") is not None
            ]),
            "power_raw_abs_err_max": safe_max([
                float(r["abs_err_raw"]) for r in compare_rows
                if r.get("abs_err_raw") is not None
            ]),
        },
        "mqtt": {
            "connect_ok": int(mqtt_stats["connect_ok"]),
            "connect_fail": int(mqtt_stats["connect_fail"]),
            "subscribed": int(mqtt_stats["subscribed"]),
            "tx_frames": int(mqtt_stats["tx_frames"]),
            "rx_frames": int(mqtt_stats["rx_frames"]),
            "telemetry_queued": int(mqtt_stats["telemetry_queued"]),
            "heartbeat_queued": int(mqtt_stats["heartbeat_queued"]),
            "cmd_ack_queued": int(mqtt_stats["cmd_ack_queued"]),
            "publish_fail": int(mqtt_stats["publish_fail"]),
            "telemetry_fail": int(mqtt_stats["telemetry_fail"]),
            "heartbeat_fail": int(mqtt_stats["heartbeat_fail"]),
            "cmd_ack_fail": int(mqtt_stats["cmd_ack_fail"]),
            "activation_ok": int(mqtt_stats["activation_ok"]),
            "activation_fail": int(mqtt_stats["activation_fail"]),
        },
        "system_resource": {
            "samples": len(sys_rows),
            "cpu_sys_avg_pct": safe_mean([float(r["cpu_sys_pct"]) for r in sys_rows]),
            "cpu_sys_max_pct": safe_max([float(r["cpu_sys_pct"]) for r in sys_rows]),
            "cpu_proc_avg_pct": safe_mean([float(r["cpu_proc_pct"]) for r in sys_rows]),
            "temp_max_c": safe_max([float(r["temp_c"]) for r in sys_rows if r["temp_c"] is not None]),
            "rss_max_mb": safe_max([float(r["rss_mb"]) for r in sys_rows]),
            "mem_avail_min_mb": min((int(r["mem_avail_mb"]) for r in sys_rows), default=None),
        },
        "layer_logic": {
            "serial_cmd_counts": dict(layer_cmd_stats),
        },
    }

    alerts = [
        e
        for e in data["key_events"]
        if e["severity"] in ("WARN", "ERROR")
    ]
    alerts_sorted = sorted(alerts, key=lambda x: (x["ts"], x["domain"]))
    summary["alerts"] = alerts_sorted

    return summary, data


def save_summary_json(path: str, summary: Dict[str, Any]) -> None:
    def conv(obj: Any) -> Any:
        if isinstance(obj, dt.datetime):
            return obj.strftime("%Y-%m-%d %H:%M:%S.%f")
        return obj

    with open(path, "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2, default=conv)


def parse_ts_text(s: str) -> Optional[dt.datetime]:
    try:
        return dt.datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f")
    except Exception:
        return None


def generate_plots(output_dir: str, data: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    if plt is None:
        return []

    plot_dir = os.path.join(output_dir, "plots")
    os.makedirs(plot_dir, exist_ok=True)
    generated: List[str] = []

    def draw_series(
        rows: List[Dict[str, Any]],
        y_keys: List[str],
        title: str,
        filename: str,
        ylabel: str = "",
    ) -> None:
        xs: List[dt.datetime] = []
        ys: Dict[str, List[float]] = {k: [] for k in y_keys}
        for row in rows:
            t = parse_ts_text(str(row.get("ts", "")))
            if not t:
                continue
            xs.append(t)
            for k in y_keys:
                v = row.get(k)
                if v is None or v == "":
                    ys[k].append(float("nan"))
                else:
                    try:
                        ys[k].append(float(v))
                    except Exception:
                        ys[k].append(float("nan"))

        if not xs:
            return

        fig, ax = plt.subplots(figsize=(12, 4))
        for k in y_keys:
            ax.plot(xs, ys[k], label=k, linewidth=1.2)
        ax.set_title(title)
        ax.set_xlabel("time")
        if ylabel:
            ax.set_ylabel(ylabel)
        ax.grid(True, linestyle="--", alpha=0.4)
        ax.legend(loc="best")
        fig.autofmt_xdate()
        out = os.path.join(plot_dir, filename)
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        generated.append(os.path.relpath(out, output_dir).replace("\\", "/"))

    draw_series(
        data.get("metrics_env", []),
        ["O2", "T_C", "RH_pct", "PM2_5", "PM10", "P1_kPa", "P2_kPa"],
        "Environment Trends",
        "env_trends.png",
    )
    draw_series(
        data.get("metrics_camera_capture", []),
        ["committed", "slot_fail", "dequeue_fail", "release_timeout"],
        "Camera Capture + SHM Ring (5s window)",
        "camera_capture_shm.png",
    )
    draw_series(
        data.get("metrics_ui_preview", []),
        ["ok", "fail", "no_frame", "cvt_avg_us", "cvt_max_us"],
        "UI Preview Decode (5s window)",
        "ui_preview_decode.png",
    )
    draw_series(
        data.get("metrics_system_resource", []),
        ["cpu_sys_pct", "cpu_proc_pct", "temp_c", "mem_avail_mb", "rss_mb"],
        "System Resource",
        "system_resource.png",
    )
    draw_series(
        data.get("metrics_laser", []),
        ["power_pct_est", "temp_c_est", "current_a_est"],
        "Laser 3ST Trends",
        "laser_3st.png",
    )
    draw_series(
        data.get("metrics_laser_power_compare", []),
        ["setpoint_raw", "report_power_raw", "abs_err_raw"],
        "Laser Power Setpoint vs Report",
        "laser_power_compare.png",
    )

    # MQTT event count per minute
    mqtt_rows = data.get("metrics_mqtt", [])
    buckets: Dict[str, Counter] = defaultdict(Counter)
    for row in mqtt_rows:
        t = parse_ts_text(str(row.get("ts", "")))
        if not t:
            continue
        key = t.strftime("%Y-%m-%d %H:%M")
        ev = str(row.get("event", ""))
        if ev:
            buckets[key][ev] += 1
    if buckets:
        keys = sorted(buckets.keys())
        xs = [dt.datetime.strptime(k, "%Y-%m-%d %H:%M") for k in keys]
        y_ok = [buckets[k].get("telemetry_queued", 0) + buckets[k].get("heartbeat_queued", 0) + buckets[k].get("cmd_ack_queued", 0) for k in keys]
        y_fail = [buckets[k].get("telemetry_fail", 0) + buckets[k].get("heartbeat_fail", 0) + buckets[k].get("cmd_ack_fail", 0) for k in keys]
        fig, ax = plt.subplots(figsize=(12, 4))
        ax.plot(xs, y_ok, label="mqtt_publish_ok", linewidth=1.2)
        ax.plot(xs, y_fail, label="mqtt_publish_fail", linewidth=1.2)
        ax.set_title("MQTT Publish Events Per Minute")
        ax.grid(True, linestyle="--", alpha=0.4)
        ax.legend(loc="best")
        fig.autofmt_xdate()
        out = os.path.join(plot_dir, "mqtt_publish_per_min.png")
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        generated.append(os.path.relpath(out, output_dir).replace("\\", "/"))

    return generated


def generate_env_interactive_plot(
    output_dir: str,
    data: Dict[str, List[Dict[str, Any]]],
) -> Optional[str]:
    if go is None or make_subplots is None:
        return None

    rows = data.get("metrics_env", [])
    if not rows:
        return None

    env_rows: List[Dict[str, Any]] = []
    for row in rows:
        t = parse_ts_text(str(row.get("ts", "")))
        if not t:
            continue
        env_rows.append({**row, "_dt": t})

    if not env_rows:
        return None

    env_rows.sort(key=lambda x: x["_dt"])
    xs = [r["_dt"] for r in env_rows]

    # 全量环境字段（按业务语义分组显示，避免同轴量纲差异过大）
    group1 = ["O2", "T_C", "RH_pct"]  # 气体/温湿度
    group2 = ["PM1_0", "PM2_5", "PM10"]  # 颗粒物
    group3 = ["P1_kPa", "P2_kPa", "Pfilter_kPa", "base_C"]  # 压力/温度扩展
    group4 = ["powder_bits", "powder_ok", "door_open"]  # 状态量

    fig = make_subplots(
        rows=4,
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.03,
        subplot_titles=(
            "Gas / Temperature / Humidity",
            "Particles",
            "Pressure / Base",
            "Status",
        ),
    )

    def add_group(fields: List[str], row_idx: int) -> None:
        for field in fields:
            ys: List[float] = []
            has_value = False
            for item in env_rows:
                val = item.get(field)
                if val is None or val == "":
                    ys.append(float("nan"))
                    continue
                try:
                    num = float(val)
                    ys.append(num)
                    if not math.isnan(num):
                        has_value = True
                except Exception:
                    ys.append(float("nan"))
            if not has_value:
                continue
            fig.add_trace(
                go.Scatter(
                    x=xs,
                    y=ys,
                    mode="lines+markers",
                    name=field,
                    line={"width": 1.5},
                    marker={"size": 3},
                    hovertemplate="%{x}<br>" + field + "=%{y}<extra></extra>",
                ),
                row=row_idx,
                col=1,
            )

    add_group(group1, 1)
    add_group(group2, 2)
    add_group(group3, 3)
    add_group(group4, 4)

    fig.update_layout(
        title="Environment Metrics (Interactive)",
        template="plotly_white",
        height=1200,
        hovermode="x unified",
        dragmode="zoom",
        legend={"orientation": "h", "yanchor": "bottom", "y": 1.01, "xanchor": "left", "x": 0},
        margin={"l": 60, "r": 40, "t": 90, "b": 60},
    )
    fig.update_xaxes(showgrid=True, gridcolor="rgba(0,0,0,0.1)")
    fig.update_yaxes(showgrid=True, gridcolor="rgba(0,0,0,0.1)")
    # 底部范围滑块，便于拖动查看细节
    fig.update_xaxes(rangeslider={"visible": True}, row=4, col=1)

    out = os.path.join(output_dir, "env_interactive.html")
    fig.write_html(
        out,
        include_plotlyjs="inline",
        full_html=True,
        config={
            "scrollZoom": True,  # 鼠标滚轮缩放
            "displaylogo": False,
            "responsive": True,
            "modeBarButtonsToAdd": ["drawline", "drawopenpath", "eraseshape"],
        },
    )
    return os.path.relpath(out, output_dir).replace("\\", "/")


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def load_records(files: List[str]) -> List[LogRecord]:
    records: List[LogRecord] = []
    for fp in files:
        try:
            with open(fp, "r", encoding="utf-8", errors="replace") as f:
                for i, line in enumerate(f, start=1):
                    rec = parse_log_line(line, fp, i)
                    if rec is not None:
                        records.append(rec)
        except Exception as e:
            print(f"[WARN] skip file {fp}: {e}", file=sys.stderr)
    records.sort(key=lambda r: r.ts)
    return records


def main() -> int:
    args = parse_args()
    input_dir = os.path.abspath(args.input)
    output_dir = os.path.abspath(args.output)
    ensure_dir(output_dir)

    if not os.path.isdir(input_dir):
        print(f"[ERROR] input dir not found: {input_dir}", file=sys.stderr)
        return 2

    files = read_log_files(input_dir, args.glob)
    if not files:
        print(f"[ERROR] no files matched: {os.path.join(input_dir, args.glob)}", file=sys.stderr)
        return 3

    records = load_records(files)
    if not records:
        print("[ERROR] no valid structured log lines were parsed.", file=sys.stderr)
        return 4

    summary, data = extract_from_logs(records, tz_offset=args.tz_offset)

    # Write CSV files
    key_events_csv = os.path.join(output_dir, "key_events.csv")
    metrics_env_csv = os.path.join(output_dir, "metrics_env.csv")
    metrics_laser_csv = os.path.join(output_dir, "metrics_laser.csv")
    metrics_laser_setpoint_csv = os.path.join(output_dir, "metrics_laser_setpoint.csv")
    metrics_laser_power_compare_csv = os.path.join(output_dir, "metrics_laser_power_compare.csv")
    metrics_camera_capture_csv = os.path.join(output_dir, "metrics_camera_capture.csv")
    metrics_ui_preview_csv = os.path.join(output_dir, "metrics_ui_preview.csv")
    metrics_system_resource_csv = os.path.join(output_dir, "metrics_system_resource.csv")
    metrics_mqtt_csv = os.path.join(output_dir, "metrics_mqtt.csv")
    summary_json = os.path.join(output_dir, "summary.json")

    if data["key_events"]:
        write_csv(
            key_events_csv,
            data["key_events"],
            ["severity", "domain", "ts", "msg", "file", "line_no"],
        )
    else:
        write_csv(key_events_csv, [], ["severity", "domain", "ts", "msg", "file", "line_no"])

    write_csv(
        metrics_env_csv,
        data["metrics_env"],
        [
            "ts",
            "proc",
            "kind",
            "O2",
            "P1_kPa",
            "T_C",
            "RH_pct",
            "PM1_0",
            "PM2_5",
            "PM10",
            "Pfilter_kPa",
            "P2_kPa",
            "base_C",
            "powder_bits",
            "powder_ok",
            "door_open",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_laser_csv,
        data["metrics_laser"],
        [
            "ts",
            "proc",
            "laser_type",
            "status",
            "alarm",
            "power_raw",
            "temp_raw",
            "current_raw",
            "power_pct_est",
            "temp_c_est",
            "current_a_est",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_laser_setpoint_csv,
        data["metrics_laser_setpoint"],
        [
            "ts",
            "proc",
            "source",
            "setpoint_raw",
            "feed_mm_min",
            "key",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_laser_power_compare_csv,
        data["metrics_laser_power_compare"],
        [
            "ts",
            "proc",
            "laser_type",
            "setpoint_source",
            "setpoint_raw",
            "report_power_raw",
            "delta_raw",
            "abs_err_raw",
            "report_status",
            "report_alarm",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_camera_capture_csv,
        data["metrics_camera_capture"],
        [
            "ts",
            "proc",
            "committed",
            "last_seq",
            "last_slot",
            "slot_fail",
            "dequeue_fail",
            "release_timeout",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_ui_preview_csv,
        data["metrics_ui_preview"],
        [
            "ts",
            "proc",
            "ok",
            "fail",
            "no_frame",
            "cvt_avg_us",
            "cvt_max_us",
            "loop_avg_us",
            "loop_max_us",
            "rga",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_system_resource_csv,
        data["metrics_system_resource"],
        [
            "ts",
            "proc",
            "cpu_sys_pct",
            "cpu_proc_pct",
            "temp_c",
            "mem_avail_mb",
            "mem_total_mb",
            "rss_mb",
            "file",
            "line_no",
        ],
    )
    write_csv(
        metrics_mqtt_csv,
        data["metrics_mqtt"],
        [
            "ts",
            "event",
            "topic",
            "direction",
            "payload_len",
            "qos",
            "retained",
            "level",
            "msg",
            "file",
            "line_no",
        ],
    )
    save_summary_json(summary_json, summary)

    # Plots + HTML
    plots: List[str] = []
    if not args.no_plots and plt is not None:
        plots = generate_plots(output_dir, data)
    env_interactive_html: Optional[str] = None
    if not args.no_interactive_env:
        env_interactive_html = generate_env_interactive_plot(output_dir, data)

    generated_files = {
        "summary_json": os.path.basename(summary_json),
        "key_events_csv": os.path.basename(key_events_csv),
        "metrics_env_csv": os.path.basename(metrics_env_csv),
        "metrics_laser_csv": os.path.basename(metrics_laser_csv),
        "metrics_laser_setpoint_csv": os.path.basename(metrics_laser_setpoint_csv),
        "metrics_laser_power_compare_csv": os.path.basename(metrics_laser_power_compare_csv),
        "metrics_camera_capture_csv": os.path.basename(metrics_camera_capture_csv),
        "metrics_ui_preview_csv": os.path.basename(metrics_ui_preview_csv),
        "metrics_system_resource_csv": os.path.basename(metrics_system_resource_csv),
        "metrics_mqtt_csv": os.path.basename(metrics_mqtt_csv),
        "plots": plots,
        "env_interactive_html": env_interactive_html or "",
    }
    report_path = build_html_report(summary, output_dir, generated_files)

    print(f"[OK] parsed files: {len(files)}")
    print(f"[OK] parsed lines: {len(records)}")
    print(f"[OK] output dir: {output_dir}")
    print(f"[OK] summary: {summary_json}")
    print(f"[OK] report: {report_path}")
    if env_interactive_html:
        print(f"[OK] interactive env chart: {os.path.join(output_dir, env_interactive_html)}")
    if plt is None:
        print("[INFO] matplotlib not found, skipped plot generation.")
        print("[INFO] install for plots: pip install matplotlib")
    if go is None:
        print("[INFO] plotly not found, skipped interactive environment chart.")
        print("[INFO] install for interactive chart: pip install plotly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

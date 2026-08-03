#!/usr/bin/env python3
"""
simulate_stream.py  —  真实实时流申请流程全链路模拟

同时扮演两个角色：
  设备端：以真实凭证（.device_fixture.json）连接 EMQX，
          收到 start_rtsp_push 后用指令里的 push_url 启动 ffmpeg 推流。
  C   端：调用后端 REST API（iot-command-downlink）触发开流，打印 pull_url。

⚠  设备端与 mqtt_client.py 使用同一 client_id，两者不能同时运行。
   请先停止 mqtt_client.py，再执行本脚本（默认模式 / --device-only 模式）。

依赖：pip install 'paho-mqtt>=2.0' requests

用法：
  python simulate_stream.py                  # 两端同时运行（推荐）
  python simulate_stream.py --device-only    # 只起设备端，等待后端下发指令
  python simulate_stream.py --client-only    # 只发 HTTP 请求（mqtt_client.py 已在跑）
  python simulate_stream.py --stop           # 停止实时流（DELETE）
  python simulate_stream.py --no-push        # 收到指令只打印，不启动 ffmpeg
  python simulate_stream.py --sn SNOWPOD-XXX # 覆盖 fixture 中的 SN
  python simulate_stream.py --backend http://host:8086
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
    MQTT_AVAILABLE = True
except ImportError:
    MQTT_AVAILABLE = False

try:
    import requests
    REQUESTS_AVAILABLE = True
except ImportError:
    REQUESTS_AVAILABLE = False

# ── 配置 ─────────────────────────────────────────────────────────────────────
import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).parent))
from _fixture import find_by_sn as _find_by_sn, last_entry as _last_entry

FIXTURE_FILE  = str(_Path(__file__).parent / ".device_fixture.json")  # 保留供日志打印
EMQX_HOST     = "localhost"
EMQX_PORT     = 1883
BACKEND_BASE  = "http://localhost:8086"   # iot-command-downlink
REGION        = "cn"
# ─────────────────────────────────────────────────────────────────────────────

ffmpeg_proc: subprocess.Popen | None = None
ffmpeg_lock  = threading.Lock()

# ffmpeg 测试流（彩色图案 + 440Hz 正弦波，1280x720 25fps）
FFMPEG_BASE = [
    "ffmpeg", "-re",
    "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=25",
    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
    "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
    "-b:v", "1500k", "-g", "50",
    "-c:a", "aac", "-b:a", "64k",
    "-f", "rtsp", "-rtsp_transport", "tcp",
]


# ── fixture 加载 ──────────────────────────────────────────────────────────────
def load_fixture(sn: str | None = None) -> dict:
    """按 SN 查找；未指定 SN 则取最后一条。找不到则报错退出。"""
    entry = (_find_by_sn(sn) if sn else None) or _last_entry()
    if not entry:
        print("[ERROR] .device_fixture.json 为空，请先运行 mock_mes_register.py + mock_device_activate.py")
        import sys; sys.exit(1)
    return entry


# ── ffmpeg ────────────────────────────────────────────────────────────────────
def start_ffmpeg(push_url: str):
    global ffmpeg_proc
    with ffmpeg_lock:
        if ffmpeg_proc and ffmpeg_proc.poll() is None:
            print("[DEVICE] 终止旧推流进程…")
            ffmpeg_proc.terminate()
            ffmpeg_proc.wait()
        print(f"[DEVICE] 启动 ffmpeg  →  {push_url}")
        try:
            ffmpeg_proc = subprocess.Popen(FFMPEG_BASE + [push_url])
        except FileNotFoundError:
            print("[ERROR] ffmpeg 未找到，请先安装：https://ffmpeg.org/download.html")


def stop_ffmpeg():
    global ffmpeg_proc
    with ffmpeg_lock:
        if ffmpeg_proc and ffmpeg_proc.poll() is None:
            ffmpeg_proc.terminate()
            ffmpeg_proc = None
            print("[DEVICE] 推流已停止")


# ── 设备端 MQTT ───────────────────────────────────────────────────────────────
def cmd_topic(sn: str) -> str:
    return f"snowpod/{REGION}/{sn}/cmd"

def cmd_ack_topic(sn: str) -> str:
    return f"snowpod/{REGION}/{sn}/cmd/ack"


class DeviceAgent:
    """模拟设备端：以真实凭证连接 EMQX，响应 start/stop_rtsp_push 指令。"""

    def __init__(self, sn: str, password: str, no_push: bool = False):
        self.sn       = sn
        self.no_push  = no_push
        self._ready   = threading.Event()
        self._seq     = 0

        self.client = mqtt.Client(
            client_id=sn,
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            protocol=mqtt.MQTTv5,
            reconnect_on_failure=False,
        )
        self.client.username_pw_set(username=sn, password=password)
        self.client.on_connect    = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message    = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, props=None):
        if reason_code.value == 0:
            client.subscribe(cmd_topic(self.sn), qos=1)
            self._ready.set()
            print(f"[DEVICE] ✓ MQTT 已连接  client_id={self.sn}")
            print(f"[DEVICE]   订阅 {cmd_topic(self.sn)}")
        else:
            print(f"[DEVICE] ✗ MQTT 连接失败  rc={reason_code.value}")

    def _on_disconnect(self, client, userdata, flags, reason_code, props=None):
        self._ready.clear()
        print(f"[DEVICE] MQTT 断开  rc={reason_code.value}")

    def _on_message(self, client, userdata, msg):
        try:
            envelope = json.loads(msg.payload.decode("utf-8"))
        except Exception as e:
            print(f"[DEVICE] 消息解析失败: {e}")
            return

        data    = envelope.get("data", {})
        cmd     = data.get("cmd", "")
        req_id  = envelope.get("req_id", "")

        print(f"\n[DEVICE] ← cmd={cmd}  req_id={req_id[:8]}…")

        if cmd == "start_rtsp_push":
            push_url    = data.get("push_url", "")
            stream_type = data.get("stream_type", "live")
            expires_at  = data.get("expires_at", 0)
            print(f"[DEVICE]   stream_type={stream_type}  expires_at={expires_at}")
            print(f"[DEVICE]   push_url={push_url}")
            self._ack(req_id, "start_rtsp_push", "ok")
            if self.no_push:
                print("[DEVICE]   --no-push 模式，跳过 ffmpeg")
            else:
                start_ffmpeg(push_url)

        elif cmd == "stop_rtsp_push":
            print(f"[DEVICE]   stream_type={data.get('stream_type', '')}")
            self._ack(req_id, "stop_rtsp_push", "ok")
            stop_ffmpeg()

        else:
            print(f"[DEVICE]   未处理指令，忽略")

    def _ack(self, req_id: str, cmd: str, result: str = "ok",
             reason: str | None = None, detail: dict | None = None):
        """回复标准 §二 封套格式的 command_ack。reason 与 detail 平级，位于 data 层。"""
        self._seq += 1
        data_body: dict = {"cmd": cmd, "result": result}
        if reason is not None:
            data_body["reason"] = reason      # result=rejected 时填写
        data_body["detail"] = detail or {}
        payload = {
            "v":         1,
            "type":      "command_ack",
            "device_id": self.sn,
            "ts":        int(time.time() * 1000),
            "seq":       self._seq,
            "req_id":    req_id,
            "data":      data_body,
        }
        self.client.publish(cmd_ack_topic(self.sn), json.dumps(payload), qos=1)
        print(f"[DEVICE] → ACK  cmd={cmd}  result={result}"
              + (f"  reason={reason}" if reason else "")
              + f"  topic={cmd_ack_topic(self.sn)}")

    def connect(self) -> bool:
        try:
            self.client.connect(EMQX_HOST, EMQX_PORT, keepalive=60)
            self.client.loop_start()
            return self._ready.wait(timeout=5.0)
        except Exception as e:
            print(f"[DEVICE] ✗ 连接异常: {e}")
            return False

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()


# ── C 端 HTTP ─────────────────────────────────────────────────────────────────
def api_start_live(sn: str, base: str) -> dict | None:
    url = f"{base}/api/v1/streams/{sn}/live/start"
    print(f"\n[CLIENT] POST {url}")
    try:
        resp = requests.post(url, timeout=10)
        resp.raise_for_status()
        body = resp.json()
        # 统一取 data 字段（R<T> 包装）
        return body.get("data") or body
    except requests.ConnectionError:
        print(f"[CLIENT] ✗ 无法连接 {base}  —  确认 iot-command-downlink 已启动（端口 8086）")
        return None
    except requests.HTTPError as e:
        print(f"[CLIENT] ✗ HTTP {e.response.status_code}: {e.response.text}")
        return None
    except Exception as e:
        print(f"[CLIENT] ✗ 请求失败: {e}")
        return None


def api_stop_live(sn: str, base: str):
    url = f"{base}/api/v1/streams/{sn}/live"
    print(f"\n[CLIENT] DELETE {url}")
    try:
        resp = requests.delete(url, timeout=10)
        resp.raise_for_status()
        print("[CLIENT] ✓ 停流成功")
    except Exception as e:
        print(f"[CLIENT] ✗ 停流失败: {e}")


# ── 主入口 ────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="真实实时流申请流程全链路模拟")
    parser.add_argument("--sn",          default=None,         help="覆盖 fixture 中的 SN")
    parser.add_argument("--backend",     default=BACKEND_BASE, help="后端地址（iot-command-downlink）")
    parser.add_argument("--no-push",     action="store_true",  help="设备收到指令只打印，不启动 ffmpeg")
    parser.add_argument("--device-only", action="store_true",  help="只起设备端，等待后端下发指令")
    parser.add_argument("--client-only", action="store_true",  help="只发 HTTP 请求，不连 EMQX")
    parser.add_argument("--stop",        action="store_true",  help="停止实时流（DELETE + 终止 ffmpeg）")
    args = parser.parse_args()

    if not REQUESTS_AVAILABLE and not args.device_only:
        print("[ERROR] 缺少 requests 库：pip install requests")
        sys.exit(1)
    if not MQTT_AVAILABLE and not args.client_only:
        print("[ERROR] 缺少 paho-mqtt 库：pip install 'paho-mqtt>=2.0'")
        sys.exit(1)

    # 先用 --sn 参数查 fixture，没指定则取最后一条
    fixture       = load_fixture(args.sn)
    sn            = fixture["deviceSn"]
    mqtt_password = fixture.get("mqttPassword", "")
    backend      = args.backend

    print("=" * 64)
    print(f"  设备 SN  : {sn}")
    print(f"  EMQX     : {EMQX_HOST}:{EMQX_PORT}")
    print(f"  后端     : {backend}")
    print(f"  cmd topic: {cmd_topic(sn)}")
    print("=" * 64)

    # ── 停流模式 ──────────────────────────────────────────────────────────────
    if args.stop:
        api_stop_live(sn, backend)
        stop_ffmpeg()
        return

    # ── 设备端 ────────────────────────────────────────────────────────────────
    device = None
    if not args.client_only:
        if not mqtt_password:
            print("[ERROR] fixture 中缺少 mqttPassword，请先补充或通过激活流程获取")
            sys.exit(1)
        device = DeviceAgent(sn, mqtt_password, no_push=args.no_push)
        if not device.connect():
            print("[DEVICE] ✗ 连接超时，退出")
            sys.exit(1)

    # ── C 端触发开流 ──────────────────────────────────────────────────────────
    if not args.device_only:
        time.sleep(0.3)   # 等待 MQTT 连接稳定
        data = api_start_live(sn, backend)
        if data:
            pull_url   = data.get("pull_url") or data.get("pullUrl", "")
            expires_at = data.get("expires_at") or data.get("expiresAt", 0)
            print(f"\n[CLIENT] ✓ 后端已下发 start_rtsp_push，等待设备推流上线…")
            print(f"\n[CLIENT] 拉流地址（粘贴到播放器）：")
            print(f"  {pull_url}")
            print(f"\n  vlc    : vlc \"{pull_url}\"")
            print(f"  ffplay : ffplay -rtsp_transport tcp \"{pull_url}\"")
            print(f"\n  expires_at : {expires_at}")
        else:
            if device:
                device.disconnect()
            sys.exit(1)

    # ── 保持运行，等待 MQTT 指令 / Ctrl+C ────────────────────────────────────
    if device:
        print(f"\n[DEVICE] 监听中，Ctrl+C 退出…\n")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n停止中…")
            stop_ffmpeg()
            device.disconnect()


if __name__ == "__main__":
    main()
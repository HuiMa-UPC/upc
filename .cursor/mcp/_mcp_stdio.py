#!/usr/bin/env python3
"""Cursor 本仓库 MCP 共用 NDJSON JSON-RPC 循环（非 Content-Length 分帧）。"""

from __future__ import annotations

import json
import sys
from typing import Any, Callable, Optional

PROTOCOL_VERSION = "2024-11-05"


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


def handle_initialize(msg: dict, server_name: str, server_version: str) -> None:
    send({
        "jsonrpc": "2.0",
        "id": msg["id"],
        "result": {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": server_name, "version": server_version},
        },
    })


def handle_tools_list(msg: dict, tools: dict) -> None:
    listed = []
    for name, spec in tools.items():
        listed.append({
            "name": name,
            "description": spec["description"],
            "inputSchema": spec["inputSchema"],
        })
    send({"jsonrpc": "2.0", "id": msg["id"], "result": {"tools": listed}})


def handle_tools_call(msg: dict, tools: dict) -> None:
    req_id = msg["id"]
    params = msg.get("params") or {}
    name = params.get("name", "")
    arguments = params.get("arguments") or {}

    if name not in tools:
        send({
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32602, "message": f"未知工具: {name}"},
        })
        return

    try:
        text = tools[name]["handler"](arguments)
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


def dispatch(msg: dict, tools: dict, server_name: str, server_version: str) -> None:
    method = msg.get("method", "")
    if method == "initialize":
        handle_initialize(msg, server_name, server_version)
    elif method == "notifications/initialized":
        return
    elif method == "tools/list":
        handle_tools_list(msg, tools)
    elif method == "tools/call":
        handle_tools_call(msg, tools)
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


def run_stdio(tools: dict, server_name: str, server_version: str) -> None:
    if hasattr(sys.stdin, "reconfigure"):
        sys.stdin.reconfigure(encoding="utf-8")
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    while True:
        msg = read_msg()
        if msg is None:
            break
        dispatch(msg, tools, server_name, server_version)

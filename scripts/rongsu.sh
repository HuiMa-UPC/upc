#!/bin/sh
# 统一启动 / 停止 rongsu_glavo + rongsu_ui
# 用法: rongsu.sh {start|stop|status|run}
# 环境:
#   RONGSU_BIN                    可执行文件目录，默认 /home
#   UPC_IPC_SOCKET                Glavo listen socket
#   UPC_IPC_PEER_STARTUP_WAIT_MS  等待 socket 超时，默认 10000

set -e

BIN_DIR="${RONGSU_BIN:-/home}"
GLAVO="$BIN_DIR/rongsu_glavo"
UI="$BIN_DIR/rongsu_ui"
SOCK="${UPC_IPC_SOCKET:-/tmp/upc_glavo.sock}"
WAIT_MS="${UPC_IPC_PEER_STARTUP_WAIT_MS:-10000}"

wait_for_socket() {
    sock="$1"
    label="$2"
    elapsed=0
    step=100
    while [ "$elapsed" -lt "$WAIT_MS" ]; do
        if [ -S "$sock" ]; then
            return 0
        fi
        sleep 0.1
        elapsed=$((elapsed + step))
    done
    echo "Timeout waiting for $label socket: $sock" >&2
    return 1
}

cmd="${1:-}"

start_daemon() {
    if [ ! -x "$GLAVO" ] || [ ! -x "$UI" ]; then
        echo "Missing binaries under $BIN_DIR (set RONGSU_BIN)" >&2
        exit 1
    fi
    rm -f "$SOCK"
    "$GLAVO" start
    wait_for_socket "$SOCK" "glavo"
    "$UI" start
    echo "Started glavo + ui"
}

stop_all() {
    "$UI" stop || true
    "$GLAVO" stop || true
    rm -f "$SOCK"
    echo "Stopped glavo + ui"
}

status_all() {
    echo "=== Glavo ==="
    "$GLAVO" status || true
    echo "=== UI ==="
    "$UI" status || true
}

run_foreground() {
    if [ ! -x "$GLAVO" ] || [ ! -x "$UI" ]; then
        echo "Missing binaries under $BIN_DIR" >&2
        exit 1
    fi
    rm -f "$SOCK"
    trap 'kill $GPID $UPID 2>/dev/null; rm -f "$SOCK"; exit' INT TERM

    "$GLAVO" run &
    GPID=$!
    wait_for_socket "$SOCK" "glavo"
    "$UI" run &
    UPID=$!

    wait $GPID $UPID
}

case "$cmd" in
    start)  start_daemon ;;
    stop)   stop_all ;;
    status) status_all ;;
    run)    run_foreground ;;
    *)
        echo "Usage: $0 {start|stop|status|run}" >&2
        exit 1
        ;;
esac

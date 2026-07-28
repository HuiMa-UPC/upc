#!/bin/sh
# 统一启动 / 停止 rongsu_broker + rongsu_glavo + rongsu_camera + rongsu_xfr + rongsu_visual + rongsu_ui
# 用法: rongsu.sh {start|stop|status|run}
# 环境:
#   RONGSU_BIN                    可执行文件目录，默认 /home
#   UPC_BROKER_SOCKET             Broker listen socket
#   UPC_IPC_PEER_STARTUP_WAIT_MS  等待 socket 超时，默认 10000

set -e

BIN_DIR="${RONGSU_BIN:-/home}"
BROKER="$BIN_DIR/rongsu_broker"
GLAVO="$BIN_DIR/rongsu_glavo"
UI="$BIN_DIR/rongsu_ui"
CAMERA="$BIN_DIR/rongsu_camera"
XFR="$BIN_DIR/rongsu_xfr"
VISUAL="$BIN_DIR/rongsu_visual"
BROKER_SOCK="${UPC_BROKER_SOCKET:-/tmp/upc_broker.sock}"
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
    if [ ! -x "$BROKER" ] || [ ! -x "$GLAVO" ] || [ ! -x "$UI" ] || [ ! -x "$CAMERA" ] || [ ! -x "$XFR" ] || [ ! -x "$VISUAL" ]; then
        echo "Missing binaries under $BIN_DIR (set RONGSU_BIN)" >&2
        exit 1
    fi
    rm -f "$BROKER_SOCK"
    "$BROKER" start
    wait_for_socket "$BROKER_SOCK" "broker"
    "$GLAVO" start
    "$CAMERA" start
    "$XFR" start
    "$VISUAL" start
    "$UI" start
    echo "Started broker + glavo + rongsu_camera + rongsu_xfr + rongsu_visual + ui"
}

stop_all() {
    "$UI" stop || true
    "$VISUAL" stop || true
    "$XFR" stop || true
    "$CAMERA" stop || true
    "$GLAVO" stop || true
    "$BROKER" stop || true
    rm -f "$BROKER_SOCK"
    echo "Stopped ui + rongsu_visual + rongsu_xfr + rongsu_camera + glavo + broker"
}

status_all() {
    echo "=== Broker ==="
    "$BROKER" status || true
    echo "=== Glavo ==="
    "$GLAVO" status || true
    echo "=== rongsu_camera ==="
    "$CAMERA" status || true
    echo "=== rongsu_xfr ==="
    "$XFR" status || true
    echo "=== rongsu_visual ==="
    "$VISUAL" status || true
    echo "=== UI ==="
    "$UI" status || true
}

run_foreground() {
    if [ ! -x "$BROKER" ] || [ ! -x "$GLAVO" ] || [ ! -x "$UI" ] || [ ! -x "$CAMERA" ] || [ ! -x "$XFR" ] || [ ! -x "$VISUAL" ]; then
        echo "Missing binaries under $BIN_DIR" >&2
        exit 1
    fi
    rm -f "$BROKER_SOCK"
    trap 'kill $BPID $GPID $CPID $XPID $VPID $UPID 2>/dev/null; rm -f "$BROKER_SOCK"; exit' INT TERM

    "$BROKER" run &
    BPID=$!
    wait_for_socket "$BROKER_SOCK" "broker"

    "$GLAVO" run &
    GPID=$!

    "$CAMERA" run &
    CPID=$!

    "$XFR" run &
    XPID=$!

    "$VISUAL" run &
    VPID=$!

    "$UI" run &
    UPID=$!

    wait $BPID $GPID $CPID $XPID $VPID $UPID
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

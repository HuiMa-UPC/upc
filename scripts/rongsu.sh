#!/bin/sh
# 统一启动 / 停止 rongsu_glavo + rongsu_ui
# 用法: rongsu.sh {start|stop|status|run}
# 环境: RONGSU_BIN 指向含两个可执行文件的目录，默认 /home

set -e

BIN_DIR="${RONGSU_BIN:-/home}"
GLAVO="$BIN_DIR/rongsu_glavo"
UI="$BIN_DIR/rongsu_ui"
SOCK="${UPC_IPC_SOCKET:-/tmp/upc_glavo.sock}"

cmd="${1:-}"

start_daemon() {
    if [ ! -x "$GLAVO" ] || [ ! -x "$UI" ]; then
        echo "Missing binaries under $BIN_DIR (set RONGSU_BIN)" >&2
        exit 1
    fi
    "$GLAVO" start
    sleep 0.5
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
    sleep 0.5
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

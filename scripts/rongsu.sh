#!/bin/sh
# 统一启动 / 停止 rongsu_glavo [+ test_camera] + rongsu_ui
# 用法: rongsu.sh {start|stop|status|run}
# 环境:
#   RONGSU_BIN                    可执行文件目录，默认 /home
#   UPC_IPC_SOCKET                Glavo listen socket
#   UPC_CAMERA_IPC_SOCKET         test_camera listen socket
#   UPC_IPC_PEER_STARTUP_WAIT_MS  等待 socket 超时，默认 10000
#   UPC_ENABLE_TEST_CAMERA        1|0|auto — 是否启动 test_camera（默认 auto：二进制存在则启）

set -e

BIN_DIR="${RONGSU_BIN:-/home}"
GLAVO="$BIN_DIR/rongsu_glavo"
UI="$BIN_DIR/rongsu_ui"
TEST_CAMERA="$BIN_DIR/test_camera"
SOCK="${UPC_IPC_SOCKET:-/tmp/upc_glavo.sock}"
CAM_SOCK="${UPC_CAMERA_IPC_SOCKET:-/tmp/upc_camera.sock}"
WAIT_MS="${UPC_IPC_PEER_STARTUP_WAIT_MS:-10000}"
ENABLE_TEST_CAMERA="${UPC_ENABLE_TEST_CAMERA:-auto}"

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

test_camera_enabled() {
    case "$ENABLE_TEST_CAMERA" in
        0|no|NO|false|FALSE) return 1 ;;
        1|yes|YES|true|TRUE) return 0 ;;
        auto|"") [ -x "$TEST_CAMERA" ] ;;
        *) return 1 ;;
    esac
}

start_test_camera_if_enabled() {
    if ! test_camera_enabled; then
        return 0
    fi
    if [ ! -x "$TEST_CAMERA" ]; then
        echo "UPC_ENABLE_TEST_CAMERA=1 but missing $TEST_CAMERA" >&2
        exit 1
    fi
    rm -f "$CAM_SOCK"
    "$TEST_CAMERA" start
    wait_for_socket "$CAM_SOCK" "test_camera"
}

stop_test_camera_if_present() {
    if [ -x "$TEST_CAMERA" ]; then
        "$TEST_CAMERA" stop || true
    fi
    rm -f "$CAM_SOCK"
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
    start_test_camera_if_enabled
    "$UI" start
    if test_camera_enabled; then
        echo "Started glavo + test_camera + ui"
    else
        echo "Started glavo + ui"
    fi
}

stop_all() {
    "$UI" stop || true
    stop_test_camera_if_present
    "$GLAVO" stop || true
    rm -f "$SOCK"
    echo "Stopped ui + test_camera (if any) + glavo"
}

status_all() {
    echo "=== Glavo ==="
    "$GLAVO" status || true
    if [ -x "$TEST_CAMERA" ]; then
        echo "=== test_camera ==="
        "$TEST_CAMERA" status || true
    fi
    echo "=== UI ==="
    "$UI" status || true
}

run_foreground() {
    if [ ! -x "$GLAVO" ] || [ ! -x "$UI" ]; then
        echo "Missing binaries under $BIN_DIR" >&2
        exit 1
    fi
    rm -f "$SOCK" "$CAM_SOCK"
    trap 'kill $GPID $CPID $UPID 2>/dev/null; rm -f "$SOCK" "$CAM_SOCK"; exit' INT TERM

    "$GLAVO" run &
    GPID=$!
    wait_for_socket "$SOCK" "glavo"

    CPID=""
    if test_camera_enabled; then
        if [ ! -x "$TEST_CAMERA" ]; then
            echo "UPC_ENABLE_TEST_CAMERA=1 but missing $TEST_CAMERA" >&2
            kill "$GPID" 2>/dev/null || true
            exit 1
        fi
        "$TEST_CAMERA" run &
        CPID=$!
        wait_for_socket "$CAM_SOCK" "test_camera"
    fi

    "$UI" run &
    UPID=$!

    if [ -n "$CPID" ]; then
        wait $GPID $CPID $UPID
    else
        wait $GPID $UPID
    fi
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

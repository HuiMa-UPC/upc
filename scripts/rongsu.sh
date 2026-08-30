#!/bin/sh
# 统一启动 / 停止 rongsu_broker + rongsu_glavo + rongsu_camera + rongsu_media + rongsu_xfr + rongsu_visual + rongsu_mqtt + rongsu_ui
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
MEDIA="$BIN_DIR/rongsu_media"
XFR="$BIN_DIR/rongsu_xfr"
VISUAL="$BIN_DIR/rongsu_visual"
MQTT="$BIN_DIR/rongsu_mqtt"
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

BOOTANIM_DEADLINE=/tmp/bootanim.deadline
BOOTANIM_MAX_WAIT=5

# gst-video.sh 起播时会把"最早可以打断"的 uptime 写进 $BOOTANIM_DEADLINE。
# 只补差额：上面那串服务如果已经耗掉足够时间，这里一秒都不等。
# 上限是防止 deadline 文件异常时把开机卡死。
wait_for_bootanim() {
    [ -r "$BOOTANIM_DEADLINE" ] || return 0

    remain=$(awk -v max="$BOOTANIM_MAX_WAIT" '
        NR == 1 { deadline = $1; next }
        NR == 2 {
            d = deadline - $1
            if (d < 0) d = 0
            if (d > max) d = max
            printf "%.2f", d
        }
    ' "$BOOTANIM_DEADLINE" /proc/uptime 2>/dev/null)
 
    [ -n "$remain" ] || return 0
    sleep "$remain"
}

# 开机动画通过 kmssink 占着 /dev/dri/card0 的 DRM master，rongsu_ui 要 set mode
# 就必须先让它退出。放在这里而不是放在 init 脚本里，是为了让动画一直播到 UI 真正
# 要用显示的那一刻，而不是在整串后台服务启动之前就被砍掉。
# bootanim stop 会阻塞到播放进程被杀掉才返回，所以返回时 master 已经空出来了。
stop_bootanim() {
    [ -x /usr/bin/bootanim ] || return 0
    wait_for_bootanim
    /usr/bin/bootanim stop || true
}

cmd="${1:-}"

start_daemon() {
    if [ ! -x "$BROKER" ] || [ ! -x "$GLAVO" ] || [ ! -x "$UI" ] || [ ! -x "$CAMERA" ] || [ ! -x "$MEDIA" ] || [ ! -x "$XFR" ] || [ ! -x "$VISUAL" ] || [ ! -x "$MQTT" ]; then
        echo "Missing binaries under $BIN_DIR (set RONGSU_BIN)" >&2
        exit 1
    fi
    rm -f "$BROKER_SOCK"
    "$BROKER" start
    wait_for_socket "$BROKER_SOCK" "broker"
    "$GLAVO" start
    "$CAMERA" start
    "$MEDIA" start
    "$XFR" start
    "$VISUAL" start
    "$MQTT" start
    stop_bootanim
    "$UI" start
    echo "Started broker + glavo + rongsu_camera + rongsu_media + rongsu_xfr + rongsu_visual + rongsu_mqtt + ui"
}

stop_all() {
    "$UI" stop || true
    "$MQTT" stop || true
    "$VISUAL" stop || true
    "$XFR" stop || true
    "$MEDIA" stop || true
    "$CAMERA" stop || true
    "$GLAVO" stop || true
    "$BROKER" stop || true
    rm -f "$BROKER_SOCK"
    echo "Stopped ui + rongsu_mqtt + rongsu_visual + rongsu_xfr + rongsu_media + rongsu_camera + glavo + broker"
}

status_all() {
    echo "=== Broker ==="
    "$BROKER" status || true
    echo "=== Glavo ==="
    "$GLAVO" status || true
    echo "=== rongsu_camera ==="
    "$CAMERA" status || true
    echo "=== rongsu_media ==="
    "$MEDIA" status || true
    echo "=== rongsu_xfr ==="
    "$XFR" status || true
    echo "=== rongsu_visual ==="
    "$VISUAL" status || true
    echo "=== rongsu_mqtt ==="
    "$MQTT" status || true
    echo "=== UI ==="
    "$UI" status || true
}

run_foreground() {
    if [ ! -x "$BROKER" ] || [ ! -x "$GLAVO" ] || [ ! -x "$UI" ] || [ ! -x "$CAMERA" ] || [ ! -x "$MEDIA" ] || [ ! -x "$XFR" ] || [ ! -x "$VISUAL" ] || [ ! -x "$MQTT" ]; then
        echo "Missing binaries under $BIN_DIR" >&2
        exit 1
    fi
    rm -f "$BROKER_SOCK"
    trap 'kill $BPID $GPID $CPID $MEPID $XPID $VPID $MPID $UPID 2>/dev/null; rm -f "$BROKER_SOCK"; exit' INT TERM

    "$BROKER" run &
    BPID=$!
    wait_for_socket "$BROKER_SOCK" "broker"

    "$GLAVO" run &
    GPID=$!

    "$CAMERA" run &
    CPID=$!

    "$MEDIA" run &
    MEPID=$!

    "$XFR" run &
    XPID=$!

    "$VISUAL" run &
    VPID=$!

    "$MQTT" run &
    MPID=$!

    stop_bootanim

    "$UI" run &
    UPID=$!

    wait $BPID $GPID $CPID $MEPID $XPID $VPID $MPID $UPID
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

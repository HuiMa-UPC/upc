#!/bin/sh
# 统一启动 / 停止 rongsu_broker + rongsu_glavo + rongsu_camera + rongsu_media + rongsu_xfr + rongsu_visual + rongsu_mqtt + rongsu_ui
# 用法: rongsu_app.sh {start|stop|status|run}
# 环境:
#   RONGSU_BIN                    可执行文件目录，默认 /home
#   UPC_BROKER_SOCKET             Broker listen socket
#   UPC_IPC_PEER_STARTUP_WAIT_MS  等待 socket 超时，默认 10000
#
# 本脚本随应用层 A/B 升级一起进槽（/home/APP{1,2}/rongsu_app.sh），由 /home/rongsu.sh
# 选定槽后 exec 过来。进程列表放这里而不是放 bootstrap，是为了让新增进程只影响新槽：
# 回滚到旧槽时用的是旧槽自己的这份脚本，不会去找旧槽里根本不存在的新进程。

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

# A/B 试运行确认：观察这么久之后若全部服务仍在，就认为新槽跑稳了。
BOOTSTRAP="${UPC_APP_BOOTSTRAP:-/home/rongsu.sh}"
SLOT_CONFIRM_DELAY="${UPC_SLOT_CONFIRM_DELAY:-60}"
DAEMON_NAMES="broker glavo camera media xfr visual mqtt ui"

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

# 八个守护进程是否都还活着。pid 文件路径与槽无关（见 upc_contract.hpp
# 的 process_config_*），所以回滚前后都能用同一套判断。
all_daemons_alive() {
    for name in $DAEMON_NAMES; do
        pid_file="/var/run/rongsu_$name.pid"
        [ -r "$pid_file" ] || return 1
        pid=$(cat "$pid_file" 2>/dev/null) || return 1
        [ -n "$pid" ] || return 1
        [ -d "/proc/$pid" ] || return 1
    done
    return 0
}

# 后台守护：熬过观察期且服务都在，就把 bootstrap 的 trial 标记清掉。
# 判据取"服务存活"而不是"MQTT 连上云端"，否则装在无网环境的设备永远确认不了，
# 会被 bootstrap 当成升级失败一路回滚。
# setsid 脱离当前会话，避免本脚本由 init 拉起时后台子进程被连带清掉。
spawn_slot_confirm() {
    [ -x "$BOOTSTRAP" ] || return 0
    setsid "$0" __confirm_guard >/dev/null 2>&1 &
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
    spawn_slot_confirm
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
    # 内部子命令：由 spawn_slot_confirm 起的后台守护，不对外
    __confirm_guard)
        sleep "$SLOT_CONFIRM_DELAY"
        if all_daemons_alive; then
            "$BOOTSTRAP" slot-confirm || true
        else
            echo "slot confirm skipped: some daemon died within ${SLOT_CONFIRM_DELAY}s" >&2
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|status|run}" >&2
        exit 1
        ;;
esac

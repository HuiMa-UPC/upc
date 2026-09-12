#!/bin/sh
# 应用层 A/B 槽 bootstrap：选槽 / 试运行回滚 / 交棒给槽内 rongsu_app.sh
# 用法: rongsu.sh {start|stop|status|run}
#       rongsu.sh slot-status
#       rongsu.sh slot-switch {APP1|APP2} [version]
#       rongsu.sh slot-confirm
# 环境:
#   UPC_APP_SLOT_ROOT      槽根目录，默认 /home
#   UPC_SLOT_ROLLBACK_LIMIT 连续启动多少次未确认就回滚，默认 3
#
# 本脚本刻意不出现任何进程名：进程列表随版本变化，放在槽内的 rongsu_app.sh 里跟
# 可执行文件一起升级。否则新版本新增进程时必须同时覆盖这个共享脚本，而回滚到旧槽
# 后它会去找旧槽里不存在的进程、启动失败，回滚就形同虚设。
#
# 三个 slot-* 子命令是 OTA 侧唯一的槽管理入口（写入口收敛在这里，MQTT 只读状态
# 文件），语义一旦定下不要改：旧槽里的脚本也会调用它们。

set -e

SLOT_ROOT="${UPC_APP_SLOT_ROOT:-/home}"
STATE_FILE="$SLOT_ROOT/app_slot"
SLOT_A=APP1
SLOT_B=APP2

# 连续启动这么多次仍未被 slot-confirm 确认，就判定新槽不可用并回滚。
# 取 3 而不是 1，是为了容忍偶发的开机异常（外设未就绪等）导致的单次启动失败，
# 不至于把本来没问题的新版本误回滚掉。
ROLLBACK_BOOT_LIMIT="${UPC_SLOT_ROLLBACK_LIMIT:-3}"

is_valid_slot() {
    [ "$1" = "$SLOT_A" ] || [ "$1" = "$SLOT_B" ]
}

# 读单个字段，缺文件 / 缺字段时给默认值，保证状态文件损坏也能开机
state_get() {
    if [ ! -r "$STATE_FILE" ]; then
        echo "$2"
        return 0
    fi
    v=$(sed -n "s/^$1=//p" "$STATE_FILE" 2>/dev/null | head -n 1)
    if [ -n "$v" ]; then
        echo "$v"
    else
        echo "$2"
    fi
}

# 整体重写状态文件：先写 .tmp 再 mv，避免掉电时留下半截文件导致下次开机选不出槽
# 参数: active prev trial boot_count rollback version
state_write() {
    tmp="$STATE_FILE.tmp"
    {
        echo "active=$1"
        echo "prev=$2"
        echo "trial=$3"
        echo "boot_count=$4"
        echo "rollback=$5"
        echo "version=$6"
    } > "$tmp"
    sync "$tmp" 2>/dev/null || sync
    mv -f "$tmp" "$STATE_FILE"
}

# 选定本次启动用的槽，必要时回滚。仅 start/run 调用：stop/status 不能算一次启动，
# 否则运维随手查几次状态就把新版本的 boot_count 耗光了。
resolve_slot_for_boot() {
    active=$(state_get active "$SLOT_A")
    prev=$(state_get prev "$SLOT_A")
    trial=$(state_get trial 0)
    boot_count=$(state_get boot_count 0)
    version=$(state_get version "")

    is_valid_slot "$active" || { active="$SLOT_A"; trial=0; }
    is_valid_slot "$prev" || prev="$SLOT_A"

    if [ "$trial" = "1" ] && [ "$boot_count" -ge "$ROLLBACK_BOOT_LIMIT" ]; then
        # rollback=1 是留给 MQTT 的止损信号：它据此丢弃待应用状态、不再弹
        # "是否重启升级"。否则用户点一次被回滚一次，陷入提示死循环。
        echo "slot $active unstable after $boot_count boots, rolling back to $prev" >&2
        active="$prev"
        state_write "$active" "$prev" 0 0 1 "$version"
    elif [ "$trial" = "1" ]; then
        boot_count=$((boot_count + 1))
        state_write "$active" "$prev" 1 "$boot_count" 0 "$version"
    fi
    echo "$active"
}

# 交棒：槽目录齐备就进槽，否则回退到 $SLOT_ROOT 扁平布局。
# 这个回退分支是给还没做 A/B 迁移的老设备和开发机留的，缺了它，脚本一升级
# 就会在没有 APP1/APP2 的机器上直接起不来。
exec_slot_script() {
    slot="$1"
    shift
    app_script="$SLOT_ROOT/$slot/rongsu_app.sh"
    bin_dir="$SLOT_ROOT/$slot"
    if [ ! -x "$app_script" ]; then
        app_script="$SLOT_ROOT/rongsu_app.sh"
        bin_dir="$SLOT_ROOT"
    fi
    if [ ! -x "$app_script" ]; then
        echo "Missing rongsu_app.sh under $SLOT_ROOT/$slot or $SLOT_ROOT" >&2
        exit 1
    fi
    RONGSU_BIN="$bin_dir" UPC_APP_BOOTSTRAP="$0" exec "$app_script" "$@"
}

cmd="${1:-}"

case "$cmd" in
    start|run)
        slot=$(resolve_slot_for_boot)
        exec_slot_script "$slot" "$cmd"
        ;;
    stop|status)
        slot=$(state_get active "$SLOT_A")
        is_valid_slot "$slot" || slot="$SLOT_A"
        exec_slot_script "$slot" "$cmd"
        ;;
    slot-status)
        echo "active=$(state_get active "$SLOT_A")"
        echo "prev=$(state_get prev "$SLOT_A")"
        echo "trial=$(state_get trial 0)"
        echo "boot_count=$(state_get boot_count 0)"
        echo "rollback=$(state_get rollback 0)"
        echo "version=$(state_get version "")"
        ;;
    slot-switch)
        target="${2:-}"
        if ! is_valid_slot "$target"; then
            echo "Usage: $0 slot-switch {$SLOT_A|$SLOT_B} [version]" >&2
            exit 1
        fi
        cur=$(state_get active "$SLOT_A")
        is_valid_slot "$cur" || cur="$SLOT_A"
        if [ "$target" = "$cur" ]; then
            echo "Already on $target" >&2
            exit 1
        fi
        # prev 记当前槽：它刚刚被证明能跑起来，是唯一可信的回滚目标
        state_write "$target" "$cur" 1 0 0 "${3:-}"
        sync
        echo "Switched to $target (trial), previous $cur"
        ;;
    slot-confirm)
        active=$(state_get active "$SLOT_A")
        is_valid_slot "$active" || active="$SLOT_A"
        # 确认之后 prev 指向 active：下次升级的回滚目标就是这个已验证的版本
        state_write "$active" "$active" 0 0 0 "$(state_get version "")"
        ;;
    *)
        echo "Usage: $0 {start|stop|status|run|slot-status|slot-switch <APP1|APP2>|slot-confirm}" >&2
        exit 1
        ;;
esac

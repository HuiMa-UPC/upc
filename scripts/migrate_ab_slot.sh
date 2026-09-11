#!/bin/sh
# 把设备从扁平布局（/home/rongsu_* + /home/rongsu.sh）迁到应用层 A/B 槽布局。
#
# 用法:
#   migrate_ab_slot.sh [源目录]        执行迁移，源目录默认为脚本所在目录
#   migrate_ab_slot.sh --cleanup       迁移验证无误后，删掉 /home 顶层的旧副本
#
# 源目录需含新版 rongsu.sh（bootstrap）与 rongsu_app.sh（槽内启动脚本）。
#
# 迁移顺序是刻意设计的：先把二进制**拷贝**（而不是移动）进 APP1，最后一步才
# 替换 /home/rongsu.sh。这样在 bootstrap 换掉之前的任意时刻断电，旧脚本配
# /home 顶层的原件都还能正常开机。原件默认保留，确认新布局跑通后再 --cleanup。

set -e

SLOT_ROOT="${UPC_APP_SLOT_ROOT:-/home}"
SLOT_A=APP1
SLOT_B=APP2
STATE_FILE="$SLOT_ROOT/app_slot"

BINARIES="rongsu_broker rongsu_glavo rongsu_camera rongsu_media rongsu_xfr rongsu_visual rongsu_mqtt rongsu_ui"

do_cleanup() {
    if [ ! -x "$SLOT_ROOT/$SLOT_A/rongsu_app.sh" ]; then
        echo "拒绝清理：$SLOT_ROOT/$SLOT_A 尚未就绪" >&2
        exit 1
    fi
    for b in $BINARIES; do
        if [ ! -x "$SLOT_ROOT/$SLOT_A/$b" ]; then
            echo "拒绝清理：$SLOT_ROOT/$SLOT_A/$b 缺失" >&2
            exit 1
        fi
        rm -f "$SLOT_ROOT/$b"
    done
    sync
    echo "已删除 $SLOT_ROOT 顶层的旧可执行文件副本"
    exit 0
}

if [ "${1:-}" = "--cleanup" ]; then
    do_cleanup
fi

SRC_DIR="${1:-$(cd "$(dirname "$0")" && pwd)}"
NEW_BOOTSTRAP="$SRC_DIR/rongsu.sh"
NEW_APP_SCRIPT="$SRC_DIR/rongsu_app.sh"

for f in "$NEW_BOOTSTRAP" "$NEW_APP_SCRIPT"; do
    if [ ! -f "$f" ]; then
        echo "缺少 $f" >&2
        exit 1
    fi
done

# 已经迁过就直接退出：本脚本可能被部署流程重复调用
if [ -x "$SLOT_ROOT/$SLOT_A/rongsu_app.sh" ] && [ -r "$STATE_FILE" ]; then
    echo "已是 A/B 布局，无需迁移（$(sed -n 's/^active=//p' "$STATE_FILE" | head -n 1)）"
    exit 0
fi

# 二进制来源：源目录带齐了就用源目录，否则取 /home 顶层的现役文件。
# 源目录优先是因为首次迁移通常和部署新版本同时做——设备上的现役二进制还是
# 不认识 A/B 的旧版本，把它装进 APP1 的话后续 OTA 根本不会生效。
# 只想换布局不换版本时，把源目录里的 rongsu_* 删掉即可走后一条路径。
BIN_SRC="$SRC_DIR"
for b in $BINARIES; do
    if [ ! -f "$SRC_DIR/$b" ]; then
        BIN_SRC="$SLOT_ROOT"
        break
    fi
done
for b in $BINARIES; do
    if [ ! -f "$BIN_SRC/$b" ]; then
        echo "在 $BIN_SRC 找不到 $b" >&2
        exit 1
    fi
done

echo "==> 源: $BIN_SRC  目标: $SLOT_ROOT/$SLOT_A"
mkdir -p "$SLOT_ROOT/$SLOT_A" "$SLOT_ROOT/$SLOT_B"

for b in $BINARIES; do
    cp -f "$BIN_SRC/$b" "$SLOT_ROOT/$SLOT_A/$b"
    chmod 755 "$SLOT_ROOT/$SLOT_A/$b"
done
cp -f "$NEW_APP_SCRIPT" "$SLOT_ROOT/$SLOT_A/rongsu_app.sh"
chmod 755 "$SLOT_ROOT/$SLOT_A/rongsu_app.sh"

# 校验槽内齐备后再碰启动入口
for b in $BINARIES rongsu_app.sh; do
    if [ ! -x "$SLOT_ROOT/$SLOT_A/$b" ]; then
        echo "槽内 $b 不可执行，中止（未改动启动入口）" >&2
        exit 1
    fi
done

# trial=0：APP1 装的就是现役版本，不需要试运行观察
cat > "$STATE_FILE.tmp" <<EOF
active=$SLOT_A
prev=$SLOT_A
trial=0
boot_count=0
rollback=0
version=
EOF
mv -f "$STATE_FILE.tmp" "$STATE_FILE"
sync

# 最后一步：备份旧入口并替换 bootstrap
if [ -f "$SLOT_ROOT/rongsu.sh" ] && [ ! -f "$SLOT_ROOT/rongsu.sh.bak" ]; then
    cp -f "$SLOT_ROOT/rongsu.sh" "$SLOT_ROOT/rongsu.sh.bak"
fi
cp -f "$NEW_BOOTSTRAP" "$SLOT_ROOT/rongsu.sh.new"
chmod 755 "$SLOT_ROOT/rongsu.sh.new"
mv -f "$SLOT_ROOT/rongsu.sh.new" "$SLOT_ROOT/rongsu.sh"
sync

echo "==> 迁移完成"
"$SLOT_ROOT/rongsu.sh" slot-status
echo ""
echo "$SLOT_ROOT 顶层的旧可执行文件已保留作为回退。"
echo "重启验证新布局能正常启动后，执行 $0 --cleanup 释放空间。"

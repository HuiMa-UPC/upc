#!/usr/bin/env bash
# 全量编译 rongsu_upc（UI / Glavo / Camera / Media / Xfer / Visual / MQTT / Broker 等全部目标）
#
# 用法:
#   ./build.sh              配置（若尚未配置）并编译
#   ./build.sh clean        删除 build/ 与 output/（含 install/）
# 环境变量（可选）:
#   BUILD_DIR     构建目录，默认 ./build
#   OUTPUT_DIR    产物输出目录，默认 ./output
#   BUILD_TYPE    Release | Debug | RelWithDebInfo | MinSizeRel，默认 Release
#   JOBS          并行数，默认 nproc
#   UPC_DEBUG     ON|OFF，默认 ON
#   TOOLCHAIN     覆盖根 CMakeLists.txt 中 set(TOOLCHAIN ...)；未设则跟 CMakeLists

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/output}"
INSTALL_DIR="${OUTPUT_DIR}/install"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
UPC_DEBUG="${UPC_DEBUG:-ON}"

# 从根 CMakeLists.txt 读取未注释的 set(TOOLCHAIN xxx)
read_cmake_toolchain() {
    local line name
    line="$(grep -E '^[[:space:]]*set\(TOOLCHAIN[[:space:]]+' \
        "$ROOT/CMakeLists.txt" | head -n1 || true)"
    if [[ -z "$line" ]]; then
        echo "错误: 根 CMakeLists.txt 中未找到 set(TOOLCHAIN ...)" >&2
        exit 1
    fi
    name="${line#*set(TOOLCHAIN }"
    name="${name%%)*}"
    name="${name%%[[:space:]]*}"
    name="${name//[[:space:]]/}"
    if [[ -z "$name" ]]; then
        echo "错误: 无法解析 CMakeLists.txt 中的 TOOLCHAIN" >&2
        exit 1
    fi
    printf '%s' "$name"
}

TOOLCHAIN="${TOOLCHAIN:-$(read_cmake_toolchain)}"
TOOLCHAIN_FILE="$ROOT/cmake/${TOOLCHAIN}.cmake"

cmd="${1:-build}"

list_toolchains() {
    local f
    for f in "$ROOT"/cmake/*-toolchain.cmake; do
        [[ -f "$f" ]] || continue
        basename "$f" .cmake
    done | paste -sd ', ' -
}

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    echo "错误: 找不到工具链文件 $TOOLCHAIN_FILE" >&2
    echo "可用: $(list_toolchains)" >&2
    exit 1
fi

read_toolchain_compilers() {
    local line
    line="$(grep -E '^set\(CMAKE_C_COMPILER ' "$TOOLCHAIN_FILE" | head -n1)"
    CMAKE_C_COMPILER="${line#set(CMAKE_C_COMPILER }"
    CMAKE_C_COMPILER="${CMAKE_C_COMPILER%)}"

    line="$(grep -E '^set\(CMAKE_CXX_COMPILER ' "$TOOLCHAIN_FILE" | head -n1)"
    CMAKE_CXX_COMPILER="${line#set(CMAKE_CXX_COMPILER }"
    CMAKE_CXX_COMPILER="${CMAKE_CXX_COMPILER%)}"

    if [[ -z "${CMAKE_C_COMPILER:-}" || -z "${CMAKE_CXX_COMPILER:-}" ]]; then
        echo "错误: 无法从 $TOOLCHAIN_FILE 解析编译器路径" >&2
        exit 1
    fi
}

read_toolchain_compilers

do_configure() {
    echo "==> cmake 配置: BUILD_DIR=$BUILD_DIR BUILD_TYPE=$BUILD_TYPE TOOLCHAIN=$TOOLCHAIN UPC_DEBUG=$UPC_DEBUG"
    cmake --no-warn-unused-cli -B "$BUILD_DIR" -S "$ROOT" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE:STRING="$BUILD_TYPE" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_C_COMPILER:FILEPATH="$CMAKE_C_COMPILER" \
        -DCMAKE_CXX_COMPILER:FILEPATH="$CMAKE_CXX_COMPILER" \
        -DUPC_DEBUG="$UPC_DEBUG"
}

do_build() {
    echo "==> cmake 编译: -j$JOBS BUILD_TYPE=$BUILD_TYPE"
    cmake --build "$BUILD_DIR" --parallel "$JOBS" --config "$BUILD_TYPE"
}

do_collect_outputs() {
    mkdir -p "$OUTPUT_DIR"

    local -a artifacts=(
        "app/broker/rongsu_broker"
        "app/ui/rongsu_ui"
        "app/glavo/rongsu_glavo"
        "app/camera/rongsu_camera"
        "app/media/rongsu_media"
        "app/xfer/rongsu_xfr"
        "app/visual/rongsu_visual"
        "app/mqtt/rongsu_mqtt"
    )

    echo "==> 复制可执行文件到 $OUTPUT_DIR"
    local rel name src
    for rel in "${artifacts[@]}"; do
        src="$BUILD_DIR/$rel"
        name="$(basename "$rel")"
        if [[ ! -f "$src" ]]; then
            echo "错误: 未找到 $src" >&2
            exit 1
        fi
        install -m 755 "$src" "$OUTPUT_DIR/$name"
        echo "  $name"
    done
}

do_collect_sdk() {
    rm -rf "$INSTALL_DIR"
    echo "==> 生成协作 SDK: $INSTALL_DIR"

    install_sdk_libs() {
        local pkg="$1"
        shift
        local lib_dir="$INSTALL_DIR/$pkg/lib"
        mkdir -p "$lib_dir"
        local rel base
        for rel in "$@"; do
            if [[ ! -f "$BUILD_DIR/$rel" ]]; then
                echo "错误: 未找到静态库 $BUILD_DIR/$rel" >&2
                exit 1
            fi
            base="$(basename "$rel")"
            install -m 644 "$BUILD_DIR/$rel" "$lib_dir/$base"
            echo "  $pkg/lib/$base"
        done
    }

    install_sdk_headers() {
        local pkg="$1"
        local src_root="$2"
        local inc_dir="$INSTALL_DIR/$pkg/include"
        local hdr rel
        while IFS= read -r -d '' hdr; do
            rel="${hdr#$src_root/}"
            install -D -m 644 "$hdr" "$inc_dir/$rel"
        done < <(find "$src_root" -type f \( -name '*.hpp' -o -name '*.h' \) -print0 | sort -z)
        local n
        n="$(find "$inc_dir" -type f | wc -l)"
        echo "  $pkg/include/ ($n headers)"
    }

    install_sdk_libs common "common/libupc_common.a"
    install_sdk_headers common "$ROOT/common"

    install_sdk_libs parser \
        "app/parser/libupc_parser.a" \
        "app/parser/libupc_parser_gcode.a" \
        "app/parser/libupc_parser_ovf.a"
    install_sdk_headers parser "$ROOT/app/parser"

    install_sdk_libs protocol "app/protocol/libupc_protocol.a"
    install_sdk_headers protocol "$ROOT/app/protocol"
}

# 工具链变更时必须清缓存再配置（编译器路径已写入 CMakeCache）
need_reconfigure() {
    local cache="$BUILD_DIR/CMakeCache.txt"
    local cached
    if [[ ! -f "$cache" ]]; then
        return 0
    fi
    cached="$(grep -E '^CMAKE_TOOLCHAIN_FILE:FILEPATH=' "$cache" | head -n1 || true)"
    cached="${cached#CMAKE_TOOLCHAIN_FILE:FILEPATH=}"
    if [[ -z "$cached" || "$cached" != "$TOOLCHAIN_FILE" ]]; then
        return 0
    fi
    return 1
}

case "$cmd" in
    build)
        if need_reconfigure; then
            if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
                echo "==> 工具链与 build 缓存不一致，清除 $BUILD_DIR 后重新配置"
                rm -rf "$BUILD_DIR"
            fi
            do_configure
        fi
        do_build
        do_collect_outputs
        do_collect_sdk
        echo ""
        echo "编译完成。产物目录: $OUTPUT_DIR"
        echo "协作 SDK: $INSTALL_DIR (common/ parser/ protocol/ 各含 lib/ + include/)"
        ;;
    clean)
        echo "==> 删除 $BUILD_DIR 与 $OUTPUT_DIR（含 install/）"
        rm -rf "$BUILD_DIR" "$OUTPUT_DIR"
        ;;
    *)
        echo "用法: $0 [build|clean]" >&2
        exit 1
        ;;
esac

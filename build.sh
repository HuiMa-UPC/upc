#!/usr/bin/env bash
# 全量编译 rongsu_upc（UI / Glavo / Camera / Test 等全部目标）
#
# 用法:
#   ./build.sh              配置（若尚未配置）并编译
#   ./build.sh clean        删除 build/ 与 output/ 
# 环境变量（可选）:
#   BUILD_DIR     构建目录，默认 ./build
#   OUTPUT_DIR    产物输出目录，默认 ./output
#   BUILD_TYPE    Release | Debug | RelWithDebInfo | MinSizeRel，默认 Release
#   JOBS          并行数，默认 nproc
#   UPC_DEBUG     ON|OFF，默认 ON
#   TOOLCHAIN     k1mini-buildroot-toolchain | atk-dlrk356x-toolchain

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/output}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
UPC_DEBUG="${UPC_DEBUG:-ON}"
TOOLCHAIN="${TOOLCHAIN:-k1mini-buildroot-toolchain}"
TOOLCHAIN_FILE="$ROOT/cmake/${TOOLCHAIN}.cmake"

cmd="${1:-build}"

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    echo "错误: 找不到工具链文件 $TOOLCHAIN_FILE" >&2
    echo "可用: k1mini-buildroot-toolchain, atk-dlrk356x-toolchain" >&2
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
        "app/ui/rongsu_ui"
        "app/glavo/rongsu_glavo"
        "app/camera/rongsu_camera"
        "test/camera/test_camera"
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

case "$cmd" in
    build)
        if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
            do_configure
        fi
        do_build
        do_collect_outputs
        echo ""
        echo "编译完成。产物目录: $OUTPUT_DIR"
        ;;
    clean)
        echo "==> 删除 $BUILD_DIR 与 $OUTPUT_DIR"
        rm -rf "$BUILD_DIR" "$OUTPUT_DIR"
        ;;
    *)
        echo "用法: $0 [build|clean]" >&2
        exit 1
        ;;
esac



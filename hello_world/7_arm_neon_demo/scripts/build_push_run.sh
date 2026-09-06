#!/usr/bin/env bash
# ============================================================
# build_push_run.sh — 交叉编译 + 推送 + 真机运行 NEON demo
#
# 环境覆盖: NDK=<ndk路径> ADB=<adb路径> REMOTE=<手机端目录>
#   默认: NDK=r26c, 手机端 /data/local/tmp/neon_demo
# 用法: bash scripts/build_push_run.sh
# 注意: Windows Git Bash 会把 /data/... 改写成 D:\...，故 adb 命令单独
#   加 MSYS_NO_PATHCONV=1（编译命令不能加，编译器 wrapper 依赖路径转换）。
# ============================================================
set -euo pipefail

DEMO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADB="${ADB:-D:/code_env/android_sdk/platform-tools/adb.exe}"
NDK="${NDK:-D:/code_env/android_sdk/ndk/android-ndk-r26c}"
CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android21-clang"
REMOTE="${REMOTE:-/data/local/tmp/neon_demo}"

# adb 命令专用（关掉 MSYS 路径转换）
adb_sh()   { MSYS_NO_PATHCONV=1 "$ADB" shell "$@"; }
adb_push() { MSYS_NO_PATHCONV=1 "$ADB" push "$@"; }

mkdir -p "$DEMO_DIR/build"

# 注意: Android 16 静态链接会报 "executable's TLS segment is underaligned",
#       故用动态链接（依赖手机 bionic libc，llama.cpp 同理）。
echo "==> 交叉编译 aarch64 (NEON + dotprod)"
"$CC" -O3 -march=armv8.2-a+dotprod \
      "$DEMO_DIR/src/neon_demo.c" -o "$DEMO_DIR/build/neon_demo"

echo "==> 推送 -> $REMOTE"
adb_sh mkdir -p "$REMOTE"
adb_push "$(cygpath -w "$DEMO_DIR/build/neon_demo")" "$REMOTE/"
adb_sh chmod 755 "$REMOTE/neon_demo"

echo "==> 真机运行"
adb_sh "$REMOTE/neon_demo"

#!/bin/bash
# ============================================================
# push_npu_sdk.sh — 把 llama.cpp snapdragon 构建产物推送到手机普通文件夹
#
# 关键点（本次重做的核心）:
#   之前调研一直把 SDK 看成"系统里的东西"，跑的时候依赖系统 /vendor 里的
#   libcdsprpc.so，而且从不设置 ADSP_LIBRARY_PATH，导致 CDSP 加载不到
#   HTP skel（libggml-htp-v73.so）→ 误报 "failed to open session 0x80000406"，
#   又被误诊成"fastrpc 设备被 DAC 焊死"。
#
#   正确做法: 把整个 SDK（lib + bin）当作一个普通文件夹推到 /data/local/tmp，
#   运行时 export LD_LIBRARY_PATH=./lib ADSP_LIBRARY_PATH=./lib 即可。
#   /data/local/tmp 属于 shell，adb shell 下读写无任何权限问题。
#
# 用法: bash push_npu_sdk.sh
# 前置: 本机已构建 llama.cpp pkg-adb（acc_survey/llama.cpp/pkg-adb/llama.cpp）
# 注意: Windows Git Bash 会把 /data/... 自动改写成 D:\... 路径导致 adb push 失败
#       （现象: remote secure_mkdirs() failed / std::bad_alloc），务必关掉 MSYS 路径转换。
# ============================================================
set -e
export MSYS_NO_PATHCONV=1

SDK_SRC="$(cd "$(dirname "$0")/../../../llama.cpp/pkg-adb/llama.cpp" && pwd)"
DEST="/data/local/tmp/llama.cpp"

[ -d "$SDK_SRC/lib" ] || { echo "ERROR: 找不到 $SDK_SRC（先跑 scripts/snapdragon/build.py --target adb）"; exit 1; }

echo "==> 创建目标目录 $DEST"
adb shell mkdir -p "$DEST"

echo "==> 推送 lib/ 与 bin/（约 260M，USB 需要几分钟）"
adb push "$SDK_SRC/lib" "$DEST/"
adb push "$SDK_SRC/bin" "$DEST/"

echo "==> 加执行位（adb push 会丢失 x 位，这是之前"读写权限问题"的一个来源）"
adb shell "chmod 755 $DEST/bin/* $DEST/lib/*.so"

echo "==> 校验关键文件"
adb shell "ls -la $DEST/lib/libggml-hexagon.so $DEST/lib/libggml-htp-v73.so $DEST/bin/llama-bench $DEST/bin/test-backend-ops"

echo "DONE. 手机上 SDK 位于 $DEST"

#!/bin/bash
# ============================================================
# 02_build_llama_android.sh  (共享: 供 demo1 / demo2 使用)
# 用 Android NDK 交叉编译 llama.cpp -> arm64-v8a
# 产物: llama-idle, llama-bench, llama-simple 及所需 .so (在 llama.cpp/build-android/bin/)
# 之后用 03_push_to_phone.sh 推送到手机。
#
# 用法:
#   bash scripts/02_build_llama_android.sh
#
# 前置:
#   - cmake / ninja 可用 (Windows 可: pip install cmake ninja)
#   - 环境变量 ANDROID_NDK 指向 NDK 根目录, 或修改下方 NDK 默认值
#
# 说明: 本版本 llama.cpp 已将 llama-cli 更名为 llama-idle;
#       关闭 LLAMA_BUILD_SERVER/APP 以跳过 Web UI 的 host 编译。
# ============================================================
set -euo pipefail

# ---- 可配置项 ----
LLAMA_DIR="${LLAMA_DIR:-/d/code/PYCHARM_PROJECT/acc_survey/llama.cpp}"
NDK="${ANDROID_NDK:-/d/code_env/android_sdk/ndk/android-ndk-r26c}"
ABI=arm64-v8a
API_LEVEL=android-28
# -------------------

echo "[1/2] cmake 配置 (ABI=$ABI, API=$API_LEVEL, NDK=$NDK)"
cd "$LLAMA_DIR"
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="$API_LEVEL" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_OPENMP=OFF \
  -DGGML_LLAMAFILE=OFF \
  -DGGML_NATIVE=OFF \
  -DLLAMA_BUILD_SERVER=OFF \
  -DLLAMA_BUILD_APP=OFF \
  -B build-android

echo "[2/2] 编译 llama-idle + llama-bench + llama-simple"
cmake --build build-android --target llama-idle llama-bench llama-simple

echo "[3/3] 拷贝产物到共享目录 binaries/ (供 03_push 使用)"
OUT="$(cd "$(dirname "$0")/.." && pwd)/binaries"
mkdir -p "$OUT"
cp -f build-android/bin/llama-simple build-android/bin/llama-bench build-android/bin/llama-idle "$OUT/"
cp -f build-android/bin/lib*.so "$OUT/"
echo "=== 产物 ($OUT) ==="
ls -lh "$OUT/"

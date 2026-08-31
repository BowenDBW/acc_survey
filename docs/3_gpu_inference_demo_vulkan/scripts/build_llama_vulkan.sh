#!/bin/bash
# ============================================================
# build_llama_vulkan.sh — 交叉编译 llama.cpp 的 Vulkan (GPU) 版
# 产物: 3_gpu_inference_demo/binaries/  (arm64-v8a, 含 libggml-vulkan.so)
#
# 与 demo0 的 CPU 版共用同一份源码, 但用独立 build 目录
#   build-android-vulkan/, 不覆盖 CPU 版产物。
#
# 前置:
#   - Vulkan SDK (glslc + SPIRV-Headers), 提供 build 期着色器编译
#   - VS 2022 Build Tools (MSVC cl) 作为宿主编译器, 用于构建 vulkan-shaders-gen
#   - NDK r26c + cmake(ninja)
# ============================================================
set -euo pipefail
export MSYS_NO_PATHCONV=1

LLAMA_DIR="${LLAMA_DIR:-/d/code/PYCHARM_PROJECT/acc_survey/llama.cpp}"
NDK="${ANDROID_NDK:-/d/code_env/android_sdk/ndk/android-ndk-r26c}"
VKSDK="${VULKAN_SDK:-C:\\VulkanSDK\\1.4.357.0}"
ABI=arm64-v8a
API=28
# cmake = pip 安装的 (python -m cmake 的 exe), ninja 独立安装
CMAKE_EXE="C:/Users/26317/AppData/Roaming/Python/Python313/site-packages/cmake/data/bin/cmake.exe"
NINJA_EXE="D:/code_env/ninjia/ninja.exe"
VCVARS="C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat"
OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)/binaries"

LLAMA_DIR_WIN="$(cygpath -w "$LLAMA_DIR")"
NDK_WIN="$(cygpath -w "$NDK")"

[ -x "$CMAKE_EXE" ] || { echo "[错误] 找不到 cmake: $CMAKE_EXE"; exit 1; }
[ -x "$NINJA_EXE" ] || { echo "[错误] 找不到 ninja: $NINJA_EXE"; exit 1; }
[ -f "$VCVARS" ]   || { echo "[错误] 找不到 vcvars64.bat: $VCVARS"; exit 1; }

echo "[1/3] 生成并运行 build_vk.bat (MSVC 环境 + NDK 交叉编译)"
# 在 cmd + vcvars64.bat 环境里执行 configure + build, 保证 vulkan-shaders-gen 能用 cl
BAT_FILE="$(cd "$(dirname "$0")" && pwd)/build_vk.bat"
cat > "$BAT_FILE" <<BAT
@echo off
call "$VCVARS" >nul || exit /b 1
set VULKAN_SDK=$VKSDK
set PATH=%PATH%;D:\code_env\ninjia
cd /d "$LLAMA_DIR_WIN" || exit /b 1
"$CMAKE_EXE" -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE="$NDK_WIN\\build\\cmake\\android.toolchain.cmake" ^
  -DANDROID_ABI=$ABI ^
  -DANDROID_PLATFORM=android-$API ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DGGML_OPENMP=OFF ^
  -DGGML_LLAMAFILE=OFF ^
  -DGGML_NATIVE=OFF ^
  -DGGML_VULKAN=ON ^
  -DCMAKE_MAKE_PROGRAM=$NINJA_EXE ^
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH ^
  -B build-android-vulkan || exit /b 1
"$CMAKE_EXE" --build build-android-vulkan --target llama-simple llama-bench || exit /b 1
BAT
BAT_WIN="$(cygpath -w "$BAT_FILE")"
cmd /c "$BAT_WIN" || { echo "[错误] Vulkan 编译失败"; exit 1; }

echo "[2/3] 拷贝产物到 $OUT_DIR"
mkdir -p "$OUT_DIR"
cp -f "$LLAMA_DIR"/build-android-vulkan/bin/llama-simple "$OUT_DIR/"
cp -f "$LLAMA_DIR"/build-android-vulkan/bin/llama-bench  "$OUT_DIR/"
cp -f "$LLAMA_DIR"/build-android-vulkan/bin/lib*.so      "$OUT_DIR/"

echo "[3/3] 产物:"
ls -lh "$OUT_DIR" | grep -E 'llama|\.so'
echo ""
echo "=== 验证是否含 Vulkan 后端 ==="
grep -c "ggml-vulkan" "$OUT_DIR/libggml.so" 2>/dev/null || true
ls "$OUT_DIR" | grep -E 'ggml-vulkan' && echo "✓ 含 libggml-vulkan.so" || echo "✗ 缺 libggml-vulkan.so"

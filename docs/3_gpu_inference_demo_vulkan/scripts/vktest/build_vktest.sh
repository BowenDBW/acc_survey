#!/bin/bash
# ============================================================
# build_vktest.sh — 编译最小 Vulkan compute 诊断工具
#  1) glslc 把 shaders/*.comp -> *.spv (与 llama.cpp 相同参数)
#  2) 生成 .spv.h 头文件 (uint32 数组)
#  3) NDK clang 交叉编译 vktest
#  4) 推送设备并运行
# ============================================================
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")"

VKSDK="C:\\VulkanSDK\\1.4.357.0"
GLSLC="/c/VulkanSDK/1.4.357.0/Bin/glslc.exe"
NDK="/d/code_env/android_sdk/ndk/android-ndk-r26c"
CLANG="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
TARGET="--target=aarch64-linux-android28"
[ -x "$GLSLC" ] || { echo "[错误] glslc: $GLSLC"; exit 1; }
[ -f "$CLANG" ] || { echo "[错误] clang: $CLANG"; exit 1; }

echo "[1/4] glslc 编译着色器 (target-env=vulkan1.2, 与 llama.cpp 非 cm2 shader 一致)"
for f in shaders/*.comp; do
    name="$(basename "$f" .comp)"
    [ -f "shaders/$name.spv" ] && continue  # 已有 .spv (如从 llama.cpp 拷贝的真实 shader) 则跳过
    "$GLSLC" -fshader-stage=compute --target-env=vulkan1.2 "$f" -o "shaders/$name.spv" \
        || { echo "[错误] 编译失败: $f"; exit 1; }
    echo "  ✓ $name.spv ($(stat -c%s "shaders/$name.spv") bytes)"
done

echo "[2/4] 生成 C 数组头文件"
python - <<'PY'
import struct, os, glob
for p in sorted(glob.glob("shaders/*.spv")):
    n = os.path.basename(p)[:-4]
    data = open(p, "rb").read()
    # 对齐到 4 字节, 按 uint32 输出
    if len(data) % 4: data += b"\x00" * (4 - len(data) % 4)
    words = struct.unpack(f"<{len(data)//4}I", data)
    with open(f"shaders/{n}.spv.h", "w") as fh:
        fh.write(",\n".join(f"0x{w:08x}u" for w in words) + "\n")
    print(f"  ok {n}.spv.h ({len(words)} words)")
PY

echo "[3/4] NDK 交叉编译 vktest"
"$CLANG" $TARGET -std=c++17 -O2 -Ishaders vktest.cpp -o vktest -lvulkan -llog
echo "  ✓ vktest ($(stat -c%s vktest) bytes)"

echo "[4/4] 推送设备并运行"
source "../../../0_environment_setup/scripts/common.sh" && locate_adb || { echo "[错误] adb"; exit 1; }
DEV=$(get_device); ADB="adb -s $DEV"
D=/data/local/tmp/vktest
$ADB shell mkdir -p "$D"
$ADB push vktest "$D/" >/dev/null
$ADB shell chmod +x "$D/vktest"
$ADB shell "$D/vktest"

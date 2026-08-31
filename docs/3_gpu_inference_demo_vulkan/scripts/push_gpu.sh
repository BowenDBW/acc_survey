#!/bin/bash
# ============================================================
# push_gpu.sh — 推送 Vulkan (GPU) 版二进制到手机
# 目标: /data/local/tmp/llama_demo_gpu/bin (独立于 CPU 版, 不互相覆盖)
# 模型: 复用共享目录 /data/local/tmp/llama_demo/models (无需重复推送 7GB)
# ============================================================
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
source "../0_environment_setup/scripts/common.sh"

locate_adb || { echo "[错误] 未找到 adb"; exit 1; }
DEV="${1:-$(get_device)}"
ADB="adb -s $DEV"
echo "[设备] $DEV"

GPU_BIN_DIR=/data/local/tmp/llama_demo_gpu/bin
MODELS_DIR=/data/local/tmp/llama_demo/models

echo "[1/3] 建立远端目录 ..."
$ADB shell mkdir -p "$GPU_BIN_DIR"

echo "[2/3] 推送 Vulkan 二进制 + 动态库 ..."
$ADB push binaries/. "$GPU_BIN_DIR/" >/dev/null
$ADB shell chmod +x "$GPU_BIN_DIR/llama-simple" "$GPU_BIN_DIR/llama-bench"

echo "[3/3] 确认模型目录存在 (复用共享模型) ..."
$ADB shell "ls $MODELS_DIR/ | head -3"

echo ""
echo "[完成] GPU 版内容:"
$ADB shell "ls -lh $GPU_BIN_DIR | grep -E 'llama|ggml-vulkan'"
echo "模型: $MODELS_DIR (共享)"

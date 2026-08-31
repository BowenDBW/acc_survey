#!/bin/bash
# ============================================================
# 03_push_to_phone.sh  (共享: 供 demo1 / demo2 使用)
# 把 0_environment_setup 下共享的编译产物和 GGUF 模型推送到手机的
#   /data/local/tmp/llama_demo/{bin,models}  (免 root, shell 用户可写)
#
# 用法:
#   bash scripts/03_push_to_phone.sh [adb设备序列号]
#   (缺省使用 adb devices 的第一台设备; 也可 export ADB_DEVICE=xxx)
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."          # 进入 0_environment_setup 目录
source "$(dirname "$0")/common.sh"

locate_adb || { echo "[错误] 未找到 adb, 请安装 platform-tools 或设置 PATH"; exit 1; }
DEV="${1:-$(get_device)}"
if [ -z "$DEV" ]; then
  echo "[错误] 未检测到 adb 设备, 请先 USB 连接并开启"USB 调试""
  exit 1
fi
ADB="adb -s $DEV"
echo "[设备] $DEV"

echo "[1/3] 建立远端目录 ..."
$ADB shell mkdir -p "$DEV_BIN_DIR" "$DEV_MODELS_DIR"

echo "[2/3] 推送二进制 + 动态库 ..."
$ADB push binaries/. "$DEV_BIN_DIR/" >/dev/null
$ADB shell chmod +x "$DEV_BIN_DIR/llama-simple" \
                      "$DEV_BIN_DIR/llama-bench" \
                      "$DEV_BIN_DIR/llama-idle"

echo "[3/3] 推送模型 (约 7.1GB, 需几分钟) ..."
$ADB push models/. "$DEV_MODELS_DIR/" >/dev/null

echo ""
echo "[完成] 手机端内容:"
$ADB shell "ls -lh $DEV_BIN_DIR $DEV_MODELS_DIR"

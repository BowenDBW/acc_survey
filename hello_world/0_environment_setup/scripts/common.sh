#!/bin/bash
# ============================================================
# common.sh — 各 demo 共享的环境辅助函数 (source 后使用)
#
#   ENV_ROOT         0_environment_setup 的绝对路径
#   locate_adb       让 adb 可用 (返回 0/1)
#   find_python      输出一个真正可用的 python 解释器路径
#   get_device       输出目标 adb 设备序列号 (来自 ADB_DEVICE 或 adb devices)
#   BIN_DIR/MODELS_DIR  手机端共享目录 (/data/local/tmp/llama_demo/...)
#
# 用法:
#   source "$(dirname "$0")/../../0_environment_setup/scripts/common.sh"
# ============================================================

# 防止 Git Bash(MSYS) 把 adb 的 /data/... 参数转成 Windows 路径
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

# 本脚本所在目录的上一级 = 0_environment_setup 根目录
COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_ROOT="$(cd "$COMMON_DIR/.." && pwd)"

# ---- 手机端共享目录 (demo1 / demo2 都往这里放) ----
DEV_BIN_DIR=/data/local/tmp/llama_demo/bin
DEV_MODELS_DIR=/data/local/tmp/llama_demo/models

# ---- 让 adb 可用 (未在 PATH 时自动定位) ----
locate_adb() {
  if ! command -v adb >/dev/null 2>&1; then
    for P in "${ANDROID_SDK_ROOT:-}/platform-tools" "${ANDROID_HOME:-}/platform-tools" \
             "/d/code_env/android_sdk/platform-tools" "${LOCALAPPDATA:-}/Android/Sdk/platform-tools"; do
      if [ -x "$P/adb.exe" ] || [ -x "$P/adb" ]; then export PATH="$P:$PATH"; break; fi
    done
  fi
  command -v adb >/dev/null 2>&1
}

# ---- 找一个真正能用的 python (Windows 的 python3 可能是 Store stub) ----
find_python() {
  for C in python python3; do
    if command -v "$C" >/dev/null 2>&1 && "$C" -c "print('py-ok')" >/dev/null 2>&1; then
      echo "$C"; return 0
    fi
  done
  return 1
}

# ---- 目标设备序列号 (ADB_DEVICE 优先, 否则取 adb devices 第一台) ----
get_device() {
  local D="${ADB_DEVICE:-}"
  if [ -z "$D" ]; then
    D=$(adb devices 2>/dev/null | sed -n '2p' | awk '{print $1}')
  fi
  echo "$D"
}

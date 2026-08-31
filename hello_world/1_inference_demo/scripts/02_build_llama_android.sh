#!/bin/bash
# ============================================================
# 02_build_llama_android.sh  (薄封装)
# NDK 交叉编译 llama.cpp 的真实实现在共享环境:
#   0_environment_setup/scripts/02_build_llama_android.sh
# ============================================================
set -euo pipefail
exec "$(dirname "$0")/../../0_environment_setup/scripts/02_build_llama_android.sh" "$@"

#!/bin/bash
# ============================================================
# 03_push_to_phone.sh  (薄封装)
# 推送二进制 + 模型到手机的真实实现在共享环境:
#   0_environment_setup/scripts/03_push_to_phone.sh
# 二进制/模型都从 0_environment_setup 推送, 落点是手机端共享目录
#   /data/local/tmp/llama_demo/{bin,models} (免 root)。
# ============================================================
set -euo pipefail
exec "$(dirname "$0")/../../0_environment_setup/scripts/03_push_to_phone.sh" "$@"

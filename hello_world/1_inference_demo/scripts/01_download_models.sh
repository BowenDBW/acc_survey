#!/bin/bash
# ============================================================
# 01_download_models.sh  (薄封装)
# 下载/校验模型的真实实现在共享环境:
#   0_environment_setup/scripts/01_download_models.sh
# 模型统一存放在 0_environment_setup/models/ (demo1 / demo2 共用)。
# ============================================================
set -euo pipefail
exec "$(dirname "$0")/../../0_environment_setup/scripts/01_download_models.sh" "$@"

#!/bin/bash
# ============================================================
# 01_download_models.sh  (共享: 供 demo1 / demo2 使用)
# 下载 Qwen3-8B / Qwen3-4B 的 GGUF 量化模型 (Q4_K_M) 到 ./models
#
# 用法:
#   bash scripts/01_download_models.sh
#
# 网络: 默认直连 huggingface.co; 若无法访问(国内网络),
#   先执行  export HF_ENDPOINT=https://hf-mirror.com  再运行本脚本
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."          # 进入 0_environment_setup 目录 (共享模型仓库)

# 支持通过环境变量 HF_ENDPOINT 切换镜像 (如 https://hf-mirror.com)
BASE="${HF_ENDPOINT:-https://huggingface.co}"

MODELS=(
  "Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf"
  "Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf"
)

mkdir -p models
for M in "${MODELS[@]}"; do
  FN="models/$(basename "$M")"
  if [ -f "$FN" ] && [ -s "$FN" ]; then
    echo "[skip] 已存在: $FN"
  else
    echo "[dl  ] $FN  <-  $BASE/$M"
    curl -L --retry 3 -C - -o "$FN" "$BASE/$M"
    echo "[done] $FN"
  fi
done

echo "=== models/ 内容 ==="
ls -lh models/

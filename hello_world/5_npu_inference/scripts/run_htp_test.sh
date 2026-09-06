#!/bin/bash
# ============================================================
# run_htp_test.sh — 在手机上用 HTP(NPU) 后端跑算子正确性测试 / 推理基准
#
# 之前误诊的根源（已被本次实测推翻）：
#   旧方案跑 llama-bench 只设 GGML_HEXAGON_DEVICES，从不设 ADSP_LIBRARY_PATH，
#   CDSP 侧靠 ADSP_LIBRARY_PATH 找 HTP skel（libggml-htp-v73.so），找不到就
#   "HTP0 failed to open session: 0x80000406"，当时被误判成"fastrpc 设备被
#   DAC 焊死、无 root 绝对走不通"，于是绕到 run-as 宿主 APK 的歪路。
#
#   实测：SDK 放在普通文件夹 /data/local/tmp/llama.cpp，设上
#     LD_LIBRARY_PATH=./lib  ADSP_LIBRARY_PATH=./lib
#   HTP0 会话直接打开，556/556 算子测试通过，Qwen3-4B 真机生成正常。
#
# 用法:
#   bash run_htp_test.sh            # 算子正确性: test-backend-ops -b HTP0
#   bash run_htp_test.sh bench      # llama-bench: pp512/tg128
# ============================================================
set -e

SDK="/data/local/tmp/llama.cpp"
MODEL="${MODEL:-$SDK/models/Qwen3-4B-Q4_K_M.gguf}"
MODE="${1:-ops}"

run() { # 在手机端统一挂上两个 *_LIBRARY_PATH
  adb shell "cd $SDK && \
    export LD_LIBRARY_PATH=./lib && \
    export ADSP_LIBRARY_PATH=./lib && \
    GGML_HEXAGON_DEVICES=HTP0 $*"
}

case "$MODE" in
  ops)
    echo "==> HTP0 算子正确性测试（对照 CPU 参考）"
    run ./bin/test-backend-ops -b HTP0 -o MUL_MAT
    ;;
  bench)
    echo "==> HTP0 推理基准（Qwen3-4B Q4_K_M, pp512/tg128）"
    run ./bin/llama-bench -m "$MODEL" --device HTP0 -ngl 99 -p 512 -n 128 -r 1
    ;;
  gen)
    echo "==> HTP0 真实文本生成"
    run ./bin/llama-simple -m "$MODEL" -n 30 -ngl 99 "The capital of France is"
    ;;
  *)
    echo "用法: $0 [ops|bench|gen]"; exit 1;;
esac

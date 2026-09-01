#!/bin/bash
# run_mllm.sh <device_serial> — 运行 TinyLlama 完整对比 benchmark
# 四档: CPU fp32 / CPU Q4_0 / GPU(fp32) / GPU(Q4_0)
# 用法: ./run_mllm.sh f6fed9b1 [num_tokens]
set -e
ADB=/d/code_env/android_sdk/platform-tools/adb.exe
DEV=${1:-f6fed9b1}
N=${2:-16}
DST=/data/local/tmp/mllm_demo
M_FP32=$DST/models/tinyllama-1.1b-chat-fp32.mllm
M_Q40=$DST/models/tinyllama-1.1b-chat-q4_0.mllm
T=$DST/models/tokenizer.json
C=$DST/models/config_tiny_llama.json
BIN=$DST/bin

run_cpu() {
  $ADB -s $DEV shell "cd $BIN && LD_LIBRARY_PATH=$BIN ./mllm-tiny-llama-bench -m $1 -mv v1 -t $T -c $C -n $N -p 'Hello world'"
}
run_gpu() {
  $ADB -s $DEV shell "cd $BIN && LD_LIBRARY_PATH=$BIN ./mllm-tiny-llama-bench-opencl -m $1 -mv v1 -t $T -c $C -n $N -p 'Hello world'"
}

echo "======================================"
echo " 1/4 CPU   fp32 ($N tok)"
echo "======================================"
run_cpu $M_FP32
echo ""
echo "======================================"
echo " 2/4 CPU   Q4_0 ($N tok)"
echo "======================================"
run_cpu $M_Q40
echo ""
echo "======================================"
echo " 3/4 GPU   fp32 ($N tok)"
echo "======================================"
run_gpu $M_FP32
echo ""
echo "======================================"
echo " 4/4 GPU   Q4_0 ($N tok)"
echo "======================================"
run_gpu $M_Q40

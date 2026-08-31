#!/bin/bash
# run_mllm.sh <device_serial> — 运行 TinyLlama CPU / OpenCL 对比 benchmark
# 用法: ./run_mllm.sh f6fed9b1 [num_tokens]
set -e
ADB=/d/code_env/android_sdk/platform-tools/adb.exe
DEV=${1:-f6fed9b1}
N=${2:-16}
DST=/data/local/tmp/mllm_demo
M=$DST/models/tinyllama-1.1b-chat-fp32.mllm
T=$DST/models/tokenizer.json
C=$DST/models/config_tiny_llama.json
BIN=$DST/bin

echo "======================================"
echo " CPU 基线 (mllm CPU, TinyLlama 1.1B fp32)"
echo "======================================"
$ADB -s $DEV shell "cd $BIN && LD_LIBRARY_PATH=$BIN ./mllm-tiny-llama-bench -m $M -mv v1 -t $T -c $C -n $N -p 'Hello world'"

echo ""
echo "======================================"
echo " GPU (OpenCL) (mllm OpenCL, TinyLlama 1.1B fp32)"
echo "======================================"
$ADB -s $DEV shell "cd $BIN && LD_LIBRARY_PATH=$BIN ./mllm-tiny-llama-bench-opencl -m $M -mv v1 -t $T -c $C -n $N -p 'Hello world'"

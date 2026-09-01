#!/bin/bash
# push_mllm.sh <device_serial> — 推送 mllm OpenCL demo 到设备
# 用法: ./push_mllm.sh f6fed9b1
set -e
ADB=/d/code_env/android_sdk/platform-tools/adb.exe
DEV=${1:-f6fed9b1}
SRC=/d/code/PYCHARM_PROJECT/acc_survey/hello_world/3_gpu_inference_demo
DST=/data/local/tmp/mllm_demo
MODEL_FP32=/d/code/PYCHARM_PROJECT/acc_survey/mllm/models/tinyllama-1.1b-chat-fp32.mllm
MODEL_Q40=$SRC/models/tinyllama-1.1b-chat-q4_0.mllm

echo "== 准备目录 =="
$ADB -s $DEV shell "mkdir -p $DST/bin $DST/models"

echo "== 推送可执行与库 =="
$ADB -s $DEV push $SRC/bin/mllm-tiny-llama-bench $DST/bin/
$ADB -s $DEV push $SRC/bin/mllm-tiny-llama-bench-opencl $DST/bin/
$ADB -s $DEV push $SRC/bin/libMllmRT.so $DST/bin/
$ADB -s $DEV push $SRC/bin/libMllmCPUBackend.so $DST/bin/
$ADB -s $DEV push $SRC/bin/libMllmOpenCLBackend.so $DST/bin/
$ADB -s $DEV push $SRC/bin/libomp.so $DST/bin/
$ADB -s $DEV push $SRC/bin/config_tiny_llama.json $DST/models/
$ADB -s $DEV push $SRC/bin/tokenizer.json $DST/models/

echo "== 推送 Q4_0 模型 (619MB) =="
$ADB -s $DEV push $MODEL_Q40 $DST/models/

echo "== 置权限 =="
$ADB -s $DEV shell "chmod 755 $DST/bin/mllm-tiny-llama-bench $DST/bin/mllm-tiny-llama-bench-opencl"

echo "== 完成，设备端内容 =="
$ADB -s $DEV shell "ls -la $DST/bin $DST/models"

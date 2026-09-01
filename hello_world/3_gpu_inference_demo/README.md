# M1 Demo: TinyLlama 1.1B 推理（mllm OpenCL vs CPU）

小米 13 Pro（SM8550 / Adreno 740，无 root）上，用 [mllm](https://github.com/UbiquitousLearning/mllm)
跑 TinyLlama 1.1B，对比 CPU backend 与 OpenCL(GPU) backend，并覆盖 fp32 / Q4_0 两种可用权重格式。

> Vulkan 方案（llama.cpp）见 `../3_gpu_inference_demo_vulkan/`，本目录仅含 mllm 方案。
> 完整性能调优过程（为什么一开始慢 7 倍、如何定位、如何 20 倍提速）见 [PERF_NOTES.md](PERF_NOTES.md)。

## 结果（设备实测，prompt "Hello world"，greedy）

| 后端 | 权重 | 模型大小 | tok/s | avg/step | prefill+first |
|---|---|---|---|---|---|
| CPU | fp32 | 4.4 GB | 1.48 | 677 ms | 3689 ms |
| **CPU** | **Q4_0** | **619 MB** | **11.1** | **90 ms** | 817 ms |
| GPU(OpenCL) | fp32 | 4.4 GB | 0.26 | 3911 ms | 5200 ms |
| **GPU(OpenCL)** | **Q4_0** | **619 MB** | **6.0** | **167 ms** | 1046 ms |
| CPU | Q8_0 | 1.17 GB | —（SIGSEGV，kGGUF 分支未实现） | | |
| GPU(OpenCL) | Q8_0 | 1.17 GB | —（SIGABRT，LinearOp 仅支持 fp32/Q4_0） | | |
| 任何 | fp16 | — | **mllm 不支持**（量化器 KAI fp16 被官方禁用） | | |

## 关键结论（诚实版）

1. **量化收益巨大（真实且可复现）**：Q4_0 相比 fp32，CPU 提速 ~7.5 倍（1.48→11.1），
   GPU 提速 ~23 倍（0.26→6.0），模型体积 4.4 GB → 619 MB。
2. **但 mllm 的 OpenCL(GPU) 并未超越其 CPU backend**（与初始预期相反）：
   - Q4_0：CPU 11.1 t/s > GPU 6.0 t/s（CPU 快 ~1.8 倍）
   - fp32：CPU 1.48 t/s > GPU 0.26 t/s（GPU 慢 ~6 倍，GEMM kernel 并行度欠载）
3. **量化深度受限**：mllm 当前只支持 fp32 与 Q4_0 两条推理路径。
   - Q8_0：CPU kGGUF 分支会 SIGSEGV；OpenCL 的 `LinearOp` 遇到非 fp32/Q4_0 权重直接 `MLLM_ERROR_EXIT`。
   - fp16：`tools/mllm-quantizer` 的 KAI fp16 impl 被官方标记 "not supported (precision error)"。
4. 生成质量：CPU 与 GPU 的 Q4_0 greedy 输出 token 序列完全一致（验证两端 kernel 正确）；
   与 fp32 相比存在量化精度引起的 greedy 漂移（符合预期）。

## 为什么 GPU 没赢（洞察）

- fp32 GEMM kernel 在 decode（M=1）时只有 ~128 个 work item，Adreno 740 严重欠载（详见 PERF_NOTES）。
- Q4_0 后 GPU 走了 decode 专用 gemv kernel（26 万 work item），但每个 work item 串行分摊 K 维，
  且整步受 in-order 队列 + 每 op 排队/同步开销影响，实测仍逊于 CPU 的 ggml NEON 量化 kernel。
- mllm 的 OpenCL backend 相对其 CPU backend 仍属早期：op 集小（无 FA2/KVCache/GQA）、
  kernel 优化有限、量化支持只有 Q4_0。

## 文件

- `bin/` — Android arm64 交叉编译产物：`mllm-tiny-llama-bench`(CPU)、`mllm-tiny-llama-bench-opencl`(GPU)、
  `libMllmRT.so` / `libMllmCPUBackend.so` / `libMllmOpenCLBackend.so` / `libomp.so`、
  `config_tiny_llama.json`、`tokenizer.json`
- `models/` — `tinyllama-1.1b-chat-q4_0.mllm`（619 MB，设备端量化产物）
- `scripts/push_mllm.sh` — 推送全部文件到设备 `/data/local/tmp/mllm_demo/`
- `scripts/run_mllm.sh` — 四档对比：CPU fp32 / CPU Q4_0 / GPU fp32 / GPU Q4_0

## 复现步骤

```bash
./scripts/push_mllm.sh <device_serial>   # 二进制 + 库 + Q4_0 模型(619MB)
./scripts/run_mllm.sh <device_serial> 32 # 四档对比，默认 16 token
```

设备端依赖：三个 `libMllm*.so` + `libomp.so`（NDK OpenMP 运行时），同目录 `LD_LIBRARY_PATH=.`。

## 备注

- 设备量化：`mllm-quantizer -i tinyllama-1.1b-chat-fp32.mllm -iv v1 -c quant_cfg_1.1b_q40.json -o ... -ov v1`
  （config 见 `examples/llama/quant_cfg_1.1b_q40.json`，Q8_0 把 `gguf_type` 改成 `Q8_0` 即可，但两端运行时均不可用）。
- OpenCL 设备能力实测：QUALCOMM Adreno，6 CU，Max WG 1024，FP16 supported，Int8 dot 不支持。

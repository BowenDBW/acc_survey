# 性能调优记录：从 GPU 慢 7 倍到 Q4_0 量化 ~20 倍提速（附量化深度对比）

本篇记录 M1 demo 的性能排查全过程。最初 OpenCL 方案比 CPU 还慢 7 倍，
经过逐 op 插桩定位，最终靠 **Q4_0 量化切换 kernel** 实现 20 倍提速。

## 1. 初始状态（为什么慢？）

| 阶段 | tok/s | avg/step | 备注 |
|---|---|---|---|
| CPU fp32（mllm CPU backend） | 1.76 | ~570 ms | 基线 |
| GPU OpenCL fp32（mllm OpenCL） | **0.26** | 3911 ms | 比 CPU 慢 7 倍 |

fp32 模型 4.4 GB 完整加载、推理结果正确，但速度灾难性。

## 2. 排查方法：给 OpenCL backend 插桩

在 mllm 源码临时加了两处计时/观测（事后已移除，干净版重建）：

1. **`OpenCLDispatcher::process()`** — 每个 op 的 reshape/setup/forward 总耗时，
   打印 `[CL-DBG] <optype> <ms> ms`（仅 >5ms）。
2. **`OpenCLX2XOp::forward()`** — CPU↔OpenCL 数据传输方向、字节数、shape，
   打印 `[X2X-DBG] <CPU|CL>-><CL|CPU> bytes=... shape=...`（受 `MLLM_OPENCL_DEBUG` 环境变量开关）。

插桩版重新交叉编译 → 推送 → 设备跑 `-n 2 -p 'Hi'` 抓完整日志。

## 3. 关键证据与误区

### 误区 1（被证伪）：权重每步重传
初看日志出现大量 `CPU->CL` 的 46 MB / 16 MB 传输，shape 正是各 Linear 权重，
一度以为是"每层每步都在把权重从 CPU 拷到 GPU"。**这是错的**：

- 这些 X2X 全部出现在 `[device] OpenCL` 打印**之前**——即 `llama.to(kOpenCL)` 加载阶段，
  是一次性上传（含 lm_head [32000,2048] 262 MB，合计 ~1.6 GB，约 2.2 s 一次性成本）。
- prefill / decode 阶段只有小 tensor 的 X2X（输入 token、RoPE、logits），**没有权重重传**。

### 真相：in-order 队列把整步耗时记在了最后一个阻塞读头上
- 每条 decode 步末尾都有一个 `CL->CPU` 读 logits（128 KB）的 X2X，耗时 **1374 ms（fp32）**。
- 128 KB 传输本身只要毫秒级；真正原因是 OpenCL 命令队列是 **in-order** 的，
  这个 blocking read（`CL_TRUE` + `finish()`）必须**等前面所有 22 层 kernel 全部执行完**，
  于是把整步 GPU 累计时间全记在它头上。fp32 prefill 末尾甚至 2562 ms。
- 所以"每 op ~100 ms"+"末尾 X2X 1.3~2.5 s"拼起来 = 22 层 fp32 kernel 的总执行时间。

### 根因：fp32 GEMM kernel 的并行度灾难
`gemm_fp32_transb_bias` 每个 work item 串行算整个 K 维的一个 16×16 tile：
- decode 时 M=1，只有 `gws = (N/16) × (M/16) = 128 × 1 = 128` 个 work item。
- Adreno 740 有 6 个 CU、每 CU 大量 SIMD lane，128 个 work item 连一个 CU 都填不满 → 严重欠载。
- 实测算力仅 ~1.6 GFLOP/s ≈ 峰值（~1.5 TFLOPS）的 **0.1%**。

## 4. 解决方案：Q4_0 量化，切换 decode 专用 kernel

mllm 的 OpenCL `LinearOp` 只支持 `kFloat32` 与 `kGGUF_Q4_0` 两种权重 dtype，
而 Q4_0 恰好有 decode 专用 kernel `gemv_fp32_q4_0_transb_bias`：

- `gws = N × 128`（q_proj N=2048 → **26 万个 work item**，并行度提升 2000 倍），
  128 个 work item 分摊 K 维 + local memory 归约，SUPPORTS_FP16 下 `vload_half` 解量化。
- 零改 OpenCL 代码，只需要 Q4_0 权重。

### 量化路径（无需主机 GCC/CMake）
- mllm 自带 `tools/mllm-quantizer`（GGUF Q4_0/Q8_0/Q2_K... 通用量化，无 ARM 依赖）。
- 用已配好的 Android NDK 工具链交叉编译 `mllm-quantizer`（`MLLM_ENABLE_TOOLS=ON`）。
- 在**设备上**直接量化：加载 4.4 GB fp32 → 输出 619 MB Q4_0（约 30 s，设备需 ≥5 GB 可用内存）。
  - 备选：Windows 主机 MSVC 构建失败（mllm 主 Linux 开发，MSVC 有 C++ 兼容错误），弃用。
- 配置用官方 `examples/llama/quant_cfg_1.1b_q40.json`（各 Linear 层匹配 pattern + `gguf_type=Q4_0`）。
- 量化产物：所有 Linear 权重 `GGUF_Q4_0`，norm/embed 保持 `Float32`。

## 5. 结果对比（最终修正版，含 CPU Q4_0 公平对比档）

| 配置 | 模型 | 大小 | tok/s | avg/step | prefill+first | 相对 CPU fp32 |
|---|---|---|---|---|---|---|
| CPU | fp32 | 4.4 GB | 1.48 | 677 ms | 3689 ms | 1.0× |
| CPU | Q4_0 | 619 MB | **11.1** | 90 ms | 817 ms | 7.5× |
| GPU OpenCL | fp32 | 4.4 GB | 0.26 | 3911 ms | 5200 ms | 0.18× |
| GPU OpenCL | Q4_0 | 619 MB | 6.0 | 167 ms | 1046 ms | 4.0× |

- Q4_0 相比 fp32：CPU 提速 ~7.5 倍（1.48 → 11.1），GPU 提速 ~23 倍（0.26 → 6.0），
  模型体积 4.4 GB → 619 MB（缩到 1/7）。
- **诚实结论**：mllm OpenCL(GPU) **未超越其 CPU backend**——Q4_0 下 CPU 11.1 t/s > GPU 6.0 t/s
  （CPU 快 ~1.8 倍）。早期报告中的 "GPU 反超 CPU 3.5 倍" 是 CPU fp32 vs GPU Q4_0 的不公平对比，
  补上 CPU Q4_0 档后修正，详见 §8。
- 生成质量验证：CPU 与 GPU 的 Q4_0 greedy token 序列完全一致（gemv 解量化正确）；
  与 fp32 相比存在量化精度引起的 greedy 漂移（符合预期）。

## 6. 可复用的方法论

1. **插桩后端 dispatcher + 数据传输 op**，别靠猜——"权重每步重传"的直觉就被日志证伪了。
2. **in-order 队列下，末尾 blocking read 的时间 = 前面所有 kernel 的累计时间**，别当成传输开销。
3. **移动 GPU 上，kernel 并行度 > 计算量**：M=1 的 GEMM 必须用 GEMV 专用 kernel，
   否则 6 CU 的 Adreno 740 只被 128 个 work item 占着。
4. **利用框架原生支持的量化 dtype 切换 kernel**，通常比手写 kernel 更快见效。

## 7. 后续可优化方向（未做）

- KVCache 更新（Copy 每层 2 次）与 transpose 仍有约 40% 耗时占比，可考虑 fused kernel。
- RoPE 每层重算 sin/cos 有重复，可缓存。
- 更大 batch / 更长序列时 fp32 GEMM 并行度会回升，Q4_0 相对优势主要在 decode 段。

## 8. 量化深度对比实验（fp16 / Q8_0 / Q4_0）

补充用户要求的 16bit / 8bit 实验。结论先行：**mllm 当前只支持 fp32 与 Q4_0 两条推理路径**。

### 实验设计

用 `mllm-quantizer` 在设备上额外量化了 Q8_0 变体（`quant_cfg_1.1b_q40.json` 中 `gguf_type` 改为 `Q8_0`，
生成 `tinyllama-1.1b-chat-q8_0.mllm`，1.17 GB），并尝试 fp16。

### 完整结果矩阵（设备实测，n=32，两轮稳定）

| 后端 | fp32 (4.4GB) | Q4_0 (619MB) | Q8_0 (1.17GB) | fp16 |
|---|---|---|---|---|
| CPU | 1.48 t/s | **11.1 t/s** | SIGSEGV（kGGUF 分支未实现） | 不可用 |
| GPU(OpenCL) | 0.26 t/s | **6.0 t/s** | SIGABRT（LinearOp 仅支持 fp32/Q4_0） | 不可用 |

### 各档结论

- **fp16（16bit）**：`tools/mllm-quantizer/schema/kai.cpp` 中 `QuantizeImpl_KAI_fp16_fp16_fp16p_mxk_kxn`
  的 `perform()` 直接 `MLLM_WARN("... is not supported, because it has precision error.")` 且不产出——
  **mllm 官方禁用了 fp16 量化**。即使拿到 fp16 权重，OpenCL `LinearOp` 与 CPU `LinearOp`(kDefault)
  都会断言/报错。**16bit 在 mllm 生态无路可走**（不是没试，是被框架关闭）。
- **Q8_0（8bit）**：量化本身成功，但——
  - GPU：OpenCL `LinearOp::forward` 对非 `kFloat32`/`kGGUF_Q4_0` 权重走
    `MLLM_ERROR_EXIT("Unsupported weight data type")` → SIGABRT。OpenCL kernel 目录也无任何 Q8_0 kernel。
  - CPU：`kGGUF` 分支（`K>=4` 时默认路径）调用 ggml 量化 kernel，Q4_0 正常但 **Q8_0 触发 SIGSEGV**——
    该分支的 Q8_0 路径未实现或有 bug。
- **Q4_0（4bit）**：CPU/GPU 双端可用，是 mllm 唯一可用的量化档。

### 修正后的总体结论

初始报告中"GPU 反超 CPU 3.5 倍"用的是 **CPU fp32 vs GPU Q4_0** 的不公平对比。
补上 **CPU Q4_0（11.1 t/s）** 公平对比档后，真实结论是：

- Q4_0 相比 fp32 的量化收益巨大且双端一致（CPU +7.5 倍，GPU +23 倍）；
- 但 mllm 的 OpenCL(GPU) backend 在小米 13 Pro 上**未超越其自身 CPU backend**（Q4_0 下 CPU 快 ~1.8 倍）。
- 原因是 mllm OpenCL 仍属早期：kernel 并行度/同步开销劣于 CPU 的 ggml NEON 量化 kernel，
  且量化/op 支持面窄（仅 fp32/Q4_0，无 FA2/KVCache/GQA）。

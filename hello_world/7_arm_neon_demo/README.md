# 7_arm_neon_demo — ARM NEON 在推理加速中的作用（调研 + 真机 demo）

> **状态：2026-09-06 完成。小米 13 Pro（SM8550 / aarch64）真机实测。**
> NEON 是 ARM CPU 的 128 位 SIMD 指令集。推理里它不是"可选优化"，而是
> CPU 侧量化算子的**地基**——量化 GEMM、KV cache 量化/反量化全压在上面。

---

## TL;DR — NEON 在推理加速里到底起什么作用

LLM 推理在 CPU 上的热点就三类，NEON 对每类都有决定性作用：

| 热点 | 标量写法 | NEON 写法 | 收益 |
|---|---|---|---|
| **量化 GEMM**（主力算子） | 每次算 1 个 `int8×int8` | `vdotq_s32`（SDOT）：1 条指令算 **4 组** int8 乘加 | 计算吞吐 ↑ + 权重内存带宽 ↓ 4× |
| **KV cache 量化/反量化** | 逐元素 `roundf` / 乘法 | `vcvt`+`vqmovn` 一次缩窄 8 个 / `vmovl`+`vcvt` 一次解 8 个 | swap/offload 路径的 CPU 侧吞吐 |
| **elementwise**（RMSNorm / softmax / RoPE） | 逐元素 | `vmlaq`/`vexpq`/`vmaxq` 一次 4~16 个 | 线性加速 |

一句话：**NEON 让"一次处理一个数"变成"一次处理 4~16 个数"，推理 CPU 侧
的每个热点都吃这个红利；int8 量化配合 SDOT 还能把内存带宽降到 1/4。**

---

## 与论文 mzCache [arXiv:2609.01338] 的对应

论文做的是端侧 LLM 的多任务内存管理：内存压力下把模型权重和 KV cache 逐出，
再用**统一内存**让 GPU 零等待推理、CPU 并行恢复被逐出的部分，TTFT 相比
storage-backed offload 降低 **2.1-5.5×**。

关键在论文 §5 Implementation：

> *"To enable these compression methods on the CPU, we implement new compression
> and decompression kernels using **ARM NEON SIMD instructions**, targeting the
> ARMv8-A architecture."*

即：**KV cache 的压缩（8-bit 量化）/ 解压（CacheGen）内核就是拿 NEON SIMD
写的**。恢复路径上 CPU 要极快地解压被逐出的 KV 塞回内存，这正是本 demo
①② 演示的两个内核；而③量化 GEMM 是这条恢复/推理流水线上另一个 NEON 大热点。

---

## 本机 CPU 能力（demo 启动时自检打印）

```
   ASIMD (NEON, 128bit): YES
   ASIMDDP (int8 dot) : YES
   I8MM   (int8 matmul): YES
```

SM8550 的 Cortex-X3 / A715 / A510 都支持 ARMv8.2-A 的 dot 指令，所以
`vdotq_s32` 可以放心用（编译参数 `-march=armv8.2-a+dotprod`）。

---

## 实测结果（小米 13 Pro · 2026-09-06 · 3 次运行取稳定值）

### ① KV cache 量化 f32 → int8（论文的压缩内核，16 MiB）

| 实现 | 耗时 | 加速 | 正确性 |
|---|---|---|---|
| 标量 C | 12.84 ms | — | — |
| **NEON** | 1.99 ms | **6.5×** | 与标量**逐位一致**（0/4194304 差异） |

### ② KV cache 反量化 int8 → f32（论文的解压内核，16 MiB）

| 实现 | 耗时 | 加速 | 正确性 |
|---|---|---|---|
| 标量 C | 1.90 ms | — | — |
| **NEON** | 0.93 ms | **2.0×** | 最大绝对差 = 0 |

### ③ 量化 GEMM Q8_0×Q8_0（64×4096×4096，约 2.1 GFLOP）

| 实现 | 耗时 | 吞吐 | vs 标量 |
|---|---|---|---|
| fp32 标量 C | 961 ms | 2.2 GFLOPS | — |
| fp32 NEON（`vmlaq_f32` 4 路 FMA） | 250 ms | 8.6 GFLOPS | 3.9× |
| **int8 NEON（`vdotq_s32` SDOT）** | 53.6 ms | 40.1 GFLOPS | **17.9×** |

精度：int8 相对 fp32 参考的误差 ≈ **0.53%**（Q8_0 的预期精度），可接受。

> 完整输出见 [results/run_20260906.txt](results/run_20260906.txt)，
> 明细见 [PERF_NOTES.md](PERF_NOTES.md)。
> 量化 GEMM 的加速有两份来源：SDOT 的计算吞吐 + int8 权重内存带宽只剩 1/4；
> fp32 NEON 只吃到前者，所以只有 3.9×——对比这两行就能看出"量化"那部分的价值。

---

## 目录结构

```
7_arm_neon_demo/
├── README.md            ← 本文：调研结论 + 实测
├── PERF_NOTES.md        ← 基准明细与说明
├── .gitignore           ← build/ results/ 可再生
├── src/
│   └── neon_demo.c      ← 全部内核 + 基准（单文件，标量 vs NEON 对照）
├── scripts/
│   └── build_push_run.sh← NDK 交叉编译 → adb 推送 → 真机运行
└── results/
    └── run_20260906.txt ← 一次完整运行输出
```

## 复现步骤

```bash
# 前置: 手机开 USB 调试（adb 见 0_environment_setup 的 common.sh）
bash scripts/build_push_run.sh
```

脚本做的事：NDK r26c 的 `aarch64-linux-android21-clang -O3 -march=armv8.2-a+dotprod`
交叉编译 → push 到 `/data/local/tmp/neon_demo/` → `adb shell` 运行。
（本目录的 Git Bash 脚本已处理 Windows 特有的 MSYS 路径转换和
Android 16 静态链接 TLS 对齐两个坑，见脚本注释。）

## 结论与下一步

- NEON 在 CPU 推理里是**必须有的优化**：量化 GEMM 靠 `vdotq_s32` 拿到 17.9×，
  而 KV cache 量化/反量化（论文 mzCache 的例子）拿到 2~6.5×。
- 下一步可选：把 demo ③ 换成更贴近推理的用法——权重一次性量化、激活按 block
  量化后直接复用（现在就是），或对比 `i8mm` 的 `smmmla`（本机也支持，
  见 HWCAP 自检），以及多线程（8 核 × NEON）下的 GEMM。

# PERF_NOTES — 实测明细

> 设备：小米 13 Pro（2210132C / nuwa），SM8550，Android 16，无 root。
> 运行方式：`bash scripts/build_push_run.sh`（NDK r26c，`-O3 -march=armv8.2-a+dotprod`）。
> 计时：`CLOCK_MONOTONIC`，每项 warmup + 取多次运行的最快值（best-of-N）。
> 2026-09-06 连跑 3 次，数值稳定（见下）。

## 运行 1

```
① KV 量化    标量 12.828ms | NEON 1.992ms | 6.4x | 差异 0/4194304
② KV 反量化  标量  1.906ms | NEON 0.938ms | 2.0x | 最大差 0
③ GEMM       标量 961.893ms(2.2GFLOPS) | fp32NEON 247.293ms(8.7) | int8NEON 53.575ms(40.1) | 18.0x
   int8 相对误差 0.0053
```

## 运行 2

```
① KV 量化    标量 12.840ms | NEON 1.989ms | 6.5x
② KV 反量化  标量  1.902ms | NEON 0.932ms | 2.0x
③ GEMM       标量 962.215ms(2.2) | fp32NEON 253.671ms(8.5) | int8NEON 53.634ms(40.0) | 17.9x
```

## 运行 3

```
① KV 量化    标量 12.844ms | NEON 1.991ms | 6.5x
② KV 反量化  标量  1.905ms | NEON 0.927ms | 2.1x
③ GEMM       标量 961.062ms(2.2) | fp32NEON 249.690ms(8.6) | int8NEON 53.378ms(40.2) | 18.0x
```

## 口径说明（哪些被计了时、哪些没有）

- **①② 量化/反量化**：只计内核本身（不含缓冲区分配、随机数生成）。
- **③ GEMM**：权重 `BT` 和激活 `A` 的 Q8_0 block 量化发生在计时外——
  真实推理里权重是一次性量化常驻的，激活也只在每次前向量化一次、整行 N 复用，
  所以把量化排除在 GEMM 计时外是合理的。fp32 路径同样排除转置。
- **标量基线**：用 `#pragma clang loop vectorize(disable)` 关掉了编译器自动
  向量化，对比的是「手写 NEON intrinsics」 vs 「可移植纯标量 C」，不掺编译器
  自动向量化的影响。（若让 clang 自己向量化，①②可能接近 NEON，那会弱化
  教学意义——NEON 的价值在于确定性的手写控制，而不是碰运气让编译器生成。）

## 误差

- fp32 NEON vs fp32 标量最大绝对差 ≈ 2.5e-04（`vmlaq` 是 FMA 融合，舍入不同）。
- int8 Q8_0 vs fp32 参考相对误差 ≈ 0.53%（32 元素 block、对称 [-127,127]，
  与 llama.cpp Q8_0 同款 layout 的预期精度）。

## 与 demo 5/6（NPU）如何衔接

- demo5/6 证明了 NPU/HTP 能跑：prefill 44 t/s（CPU 的 ~6×），但 **生成与 CPU
  持平**，因为每 token 有 fastrpc 提交开销、部分算子回退 CPU/OpenCL。
- 本 demo 展示的是 CPU 侧被 NPU 回退算子吃掉的底子：只要回退的算子是
  GEMM / 量化类，NEON 实现（本 demo ③/①②）就是那些回退路径的实际执行代码，
  而 SM8550 的 8 核 AArch64 加上 `i8mm`，单核已能到 40 GFLOPS。

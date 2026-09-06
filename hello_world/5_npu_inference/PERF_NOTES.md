# PERF_NOTES — 5_npu_inference 实测数据

> 设备：小米 13 Pro (2210132C / nuwa) · Android 16 · 骁龙 8 Gen 2 (SM8550) · HTP v73 ·
> **无 root** · shell (uid 2000) 直跑 · 2026-09-05
> 工具链：llama.cpp snapdragon 构建（`scripts/snapdragon/build.py --target adb`），
> `pkg-adb` 产物，推送到 `/data/local/tmp/llama.cpp`（普通文件夹）。

## 环境

```
LD_LIBRARY_PATH=./lib
ADSP_LIBRARY_PATH=./lib          ← 关键：CDSP 靠它找 libggml-htp-v73.so
GGML_HEXAGON_DEVICES=HTP0
```

## 1. 算子正确性（对照 CPU 参考）

```
test-backend-ops -b HTP0 -o MUL_MAT
556/556 tests passed
Backend HTP0: OK
```

> 全程日志：`results/2026-09-05_test-backend-ops_HTP0.log`（未入库）。

## 2. llama-bench（HTP0, Qwen3-4B Q4_K_M, 2.32 GiB, 4.02B 参数）

```
GGML_HEXAGON_DEVICES=HTP0 ./bin/llama-bench -m models/Qwen3-4B-Q4_K_M.gguf \
    --device HTP0 -ngl 99 -p 512 -n 128 -r 1

| qwen3 4B Q4_K - Medium | 2.32 GiB | 4.02 B | OpenCL,HTP | 99 | HTP0 | pp512 | 44.10 ± 0.00 |
| qwen3 4B Q4_K - Medium | 2.32 GiB | 4.02 B | OpenCL,HTP | 99 | HTP0 | tg128 |  7.27 ± 0.00 |
```

小 prompt 复核（pp64/tg32，结果一致量级）：

```
| qwen3 4B Q4_K - Medium | 2.32 GiB | 4.02 B | OpenCL,HTP | 99 | HTP0 | pp64 | 42.50 ± 0.00 |
| qwen3 4B Q4_K - Medium | 2.32 GiB | 4.02 B | OpenCL,HTP | 99 | HTP0 | tg32 |  6.87 ± 0.00 |
```

## 3. CPU 基线（demo1 同机型同模型，2026-08-30）

来源：`hello_world/1_inference_demo/README.md`（CPU/XNNPACK，`-ngl 0`）：

| 模型 | 方法 | prompt | prefill | 生成速度 |
|---|---|---|---|---|
| Qwen3-4B Q4_K_M | llama-simple | 13 tok | 1144 ms | 7.1 tok/s |
| Qwen3-4B Q4_K_M | llama-bench pp128 | 128 tok | 12363 ms | 7.3 t/s |

## 4. 对比小结

| 指标 | CPU | HTP0 (NPU) | 提升 |
|---|---|---|---|
| prefill | ~7.3 t/s | 44.10 t/s | **≈6×** |
| 生成 tg | ~7.1 tok/s | 7.27 t/s | 持平 |

- **prefill 收益巨大**：prefill 是纯计算密集（大矩阵乘在 HTP 上算），6× 提速直接砍 TTFT。
- **生成持平**：decode 是访存受限，且当前 HTP 后端部分算子回退 CPU/OpenCL
  （`backend=OpenCL,HTP`），每 token 还有 fastrpc 提交开销。可尝试
  `GGML_HEXAGON_OPPOLL` 与 `-fa`（flash attention）看是否改善。

## 5. 观察到的细节

- `FASTRPC_GET_DOMAINS query failed (0x6c), using static CDSP domains`：正常回退，
  不阻塞。
- `rpcmem_alloc2 not found, falling back to rpcmem_alloc`：系统 `libcdsprpc.so`
  版本较老，缺 `rpcmem_alloc2`，回退可用。
- `libcdsprpc.so` 仍从系统 `/vendor/lib64` 加载（pkg 内不带），但 skel
  `libggml-htp-v73.so` 必须走 `ADSP_LIBRARY_PATH`——**这就是之前缺的一环**。

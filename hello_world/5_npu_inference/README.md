# 5_npu_inference — 手机 NPU(HTP) 推理调研（重做）

> **状态：2026-09-05 重做完成。之前的结论「无 root 调 NPU 绝对走不通」被推翻。**
> 小米 13 Pro（SM8550 / HTP v73 / Android 16 / **无 root**）在纯 shell 下即可跑通 NPU。

---

## TL;DR — 之前为什么失败，正确的做法是什么

### 旧方案错在哪

之前的调研把**运行方法**搞错了，然后顺着错误结论一路绕远：

1. llama.cpp Hexagon 后端跑 `llama-bench` 时**只设了 `GGML_HEXAGON_DEVICES=HTP0`，
   从不设 `ADSP_LIBRARY_PATH`**。CDSP 侧恰恰靠 `ADSP_LIBRARY_PATH` 找 HTP skel
   （`libggml-htp-v73.so`），找不到 → `HTP0 failed to open session: error 0x80000406`。
2. 于是被**误诊**为「`/dev/adsprpc-smd` 是 `0664 system:system`，shell/app 都打不开，
   DAC 把 fastrpc 通道焊死，无 root 绝对走不通」。
3. 顺着误诊去造 **run-as 宿主 APK**（`com.example.llmbench`，`hasCode=false`，只用来拿
   run-as 壳）+ 在 `/data/local/tmp` 折腾——全都没意义，被删。

> 旧方案把「SDK 当成系统里的东西」来调：运行时依赖系统 `/vendor/lib64` 的
> `libcdsprpc.so`，又不给 CDSP 指 skel 路径。**实际上 SDK 就是一堆可执行文件 + .so，
> 放进一个普通文件夹，把 `LD_LIBRARY_PATH` / `ADSP_LIBRARY_PATH` 指过去就行**，
> 读写权限根本不是障碍。

### 正确的做法（本次实测有效）

```
手机端 /data/local/tmp/llama.cpp/          ← 普通文件夹，shell 可写可执行
├── lib/                                   ← 整个 SDK 的库 + HTP skel
│   ├── libggml-hexagon.so                 ← Hexagon 后端
│   ├── libggml-htp-v73.so                 ← v73 skel（由 CDSP 经 fastrpc 加载）
│   ├── libggml*.so / libllama*.so
└── bin/llama-bench 等

# 运行（就三行，无 root、无 APK、无 run-as）：
cd /data/local/tmp/llama.cpp
export LD_LIBRARY_PATH=./lib
export ADSP_LIBRARY_PATH=./lib        # ← 之前缺的就是这一行
GGML_HEXAGON_DEVICES=HTP0 ./bin/llama-bench -m models/Qwen3-4B-Q4_K_M.gguf --device HTP0 -ngl 99
```

## 实测结果（小米13 Pro · 无 root · Qwen3-4B Q4_K_M · HTP0）

### 1. HTP0 会话打通

```
ggml-hex: Loading driver libcdsprpc.so
ggml-hex: FASTRPC_GET_DOMAINS query failed (0x6c), using static CDSP domains
ggml-hex: Hexagon backend (experimental) : allocating new registry : ndev 1
ggml-hex: Hexagon Arch version v73
```

> fastrpc 通道对 shell 是**通的**（DSP 域查询失败只是退回静态 CDSP 域表，不阻塞）。
> 之前那条 `cat /dev/adsprpc-smd → Permission denied` 证明不了 fastrpc 不可用。

### 2. 算子正确性（对照 CPU 参考实现）

```
$ test-backend-ops -b HTP0 -o MUL_MAT
556/556 tests passed
Backend HTP0: OK
```

### 3. 推理基准 vs CPU

| 指标 | CPU（demo1 基线） | **HTP0（NPU）** | 提升 |
|---|---|---|---|
| prefill pp512 | ~7.3 t/s (pp128) | **44.10 t/s** | **~6×** |
| 生成 tg128 | ~7.1 tok/s | **7.27 t/s** | ~持平 |

> 完整记录见 [PERF_NOTES.md](PERF_NOTES.md)。生成持平是因为 HTP 后端仍在早期，
> 部分算子回退 OpenCL/CPU，且每 token 有 fastrpc 提交开销；prefill 是纯计算密集，
> NPU 优势明显。

### 4. 真实文本生成（正确性）

```
$ llama-simple -m Qwen3-4B-Q4_K_M.gguf -n 30 -ngl 99 "The capital of France is"
The capital of France is Paris. The capital of Germany is Berlin.
The capital of Italy is Rome. The capital of Spain is Madrid.
The capital of Portugal is Lisbon.
```

## 目录结构

```
5_npu_inference/
├── README.md            ← 本文：重做结论与实测
├── PERF_NOTES.md        ← benchmark 明细
├── .gitignore           ← SDK/模型可再生，不入库
└── scripts/
    ├── push_npu_sdk.sh  ← 推送 llama.cpp pkg-adb 到手机普通文件夹
    └── run_htp_test.sh  ← ops/bench/gen 三种测试入口
```

SDK 本体（`pkg-adb/llama.cpp`）与模型都在 `acc_survey/llama.cpp/`、
`hello_world/0_environment_setup/models/`，不入本目录（可再生，见 `.gitignore`）。

## 复现步骤

```bash
# 1. 推送 SDK 到手机（约 260M）
bash scripts/push_npu_sdk.sh

# 2. 算子正确性 / 基准 / 生成
bash scripts/run_htp_test.sh ops
bash scripts/run_htp_test.sh bench
bash scripts/run_htp_test.sh gen
```

## 结论与下一步

- **这台无 root 的小米 13 Pro 可以跑 NPU**，关键是把 SDK 放进普通文件夹 +
  `ADSP_LIBRARY_PATH`，不需要 system 权限、不需要 APK/run-as。
- HTP v73 只认 `libggml-htp-v73.so` 这个 skel；v75/v79/v81 是本机不用的新架构。
- 生成速度与 CPU 持平 → 下一步优化点：查哪些算子回退 CPU/OpenCL
  （`GGML_HEXAGON_OPPOLL`、op 覆盖），以及尝试 QNN 官道路径
  （ShadowNPU 的 `run_qwen_qnn.sh` 同一套 `qnn-lib` 目录 + `ADSP_LIBRARY_PATH`）。

# 3_gpu_inference_demo — GPU/NPU 推理与监测方案

> 目标：把推理从「纯 CPU (XNNPACK)」升级到「GPU (Vulkan) 推理」，并补上 GPU/NPU 的
> 监测与调用方案，延续 demo2 的「推理速度 + 芯片状态」关联思路。
> 参考：mzCache (MobiCom'26) — 手机多任务下的 LLM 内存管理，基于 llama.cpp、
> **GPU 推理 + 自研 OpenCL 注意力内核 + CPU/GPU 统一内存协同**。
> 论文原文见 `reference/mzcache-mobicom26.pdf`。

---

## 0. 从论文里我们借鉴什么（不是复制什么）

| 论文做法 | 对我们的价值 |
|---|---|
| llama.cpp + **Vulkan** 跑 GPU 推理 | **主路径**：Vulkan 是 llama.cpp 官方移动 GPU 后端，成熟可用 |
| 自研 **OpenCL 注意力内核**（0.6k 行，针对 Adreno FP16 双倍速率） | **进阶课题**：只在大 KV cache prefill 上有明显收益，初期不做 |
| CPU/GPU **统一内存 (SVM) 协同** | 理念参考；我们需要的是"GPU 干活、CPU 可并行采样" |
| 功耗 = sysfs `current × voltage` | 照抄这个测量口径（需 root） |
| GPU 内存 = KGSL `gpumem_mapped_bytes` | 照抄（需 root）；免 root 有 `dumpsys gpu` 替代 |
| 评测口径：TTFT / 功耗曲线 / 能耗(曲线下面积) | 沿用 demo2 的窗口采样 + 积分思路 |

**不复制**：mzCache 的核心是"多任务内存压力下的 KV cache 弹性驱逐/恢复"（约 6k 行 C++），
工程量大且与我们"测芯片状态"的目标无关。我们先做 **GPU 推理 + GPU/NPU 监测的基建**，
把论文的测量方法学搬过来即可。

---

## 1. 真机现状（2026-08-31 实测，小米 13 Pro / SM8550）

### 1.1 权限矩阵（决定一切方案的事实）

| 数据 | 路径 | 免 root | 说明 |
|---|---|---|---|
| CPU 三簇频率/温度/负载 | `/sys/devices/system/cpu/...` + thermal | ✅ | demo2 已在用 |
| Vulkan 推理 | NDK + libvulkan | ✅ | **Vulkan 1.3 已确认** (`android.hardware.vulkan.level=1`) |
| GPU 内存快照 | `dumpsys gpu` | ✅ | **免 root 实测可读**（见 3.1） |
| GPU 频率 `gpuclk` | `/sys/class/kgsl/kgsl-3d0/gpuclk` | ❌ | Permission denied（demo2 已实测） |
| GPU 占用 `gpu_busy_percentage` | kgsl | ❌ | Permission denied |
| GPU 内存 `gpumem_mapped_bytes` | kgsl debugfs | ❌ | `/sys/kernel/debug` 未挂载（root 才有） |
| 真实功耗 `power_now`×`voltage_now` | power_supply | ❌ | Permission denied（论文用此口径，需 root） |
| NPU 遥测（频率/负载） | — | ❌ | **无任何公开接口**（全盘 find 无 `npu` 条目） |

### 1.2 硬件/软件栈

- GPU：**Adreno 740**（`ro.hardware.egl=adreno`，驱动 `com.qualcomm.qti.gpudrivers.kalama.api33`），Vulkan 1.3
- NPU：**高通 HTP**（Hexagon Tensor Processor）软件栈完整存在：
  `libSnpeHtpV73Stub.so` / `libSnpeHtpPrepare.so` / QNN 模型（`*qnn_segqnn*`），NNAPI feature level 7
- 结论：**GPU 立刻可用（免 root）；NPU 有驱动可调，但无遥测可读**

---

## 2. 推理运行时选型

### 2.1 主后端：llama.cpp Vulkan（GGML_VULKAN=ON）✅ 推荐

- 理由：llama.cpp 官方主力移动 GPU 后端、维护活跃；论文 (mzCache) 与大量手机端工作都在用；
  Vulkan 1.3 已确认支持；我们的 llama.cpp 源码 + NDK 工具链都在（demo0 已交叉编译过 CPU 版）。
- 改动：交叉编译加 `-DGGML_VULKAN=ON`，推送新的 `llama-simple`/`llama-bench`，
  config 里 `-ngl N` 卸载层到 GPU（4B Q4_K_M ≈ 2.5GB，全量卸载可行）。
- 预期：解码从 ~8 tok/s（CPU）提升，需实测（Adreno 740 理论 FP16 约 2.6 TFLOPS 以上）。

### 2.2 OpenCL 要不要上？ → 初期不上，M5 再考虑

- llama.cpp 的 OpenCL 后端（GGML_OPENCL）**已半废弃**，Vulkan 是正路——没必要为"用 OpenCL 而用"。
- OpenCL 唯一有价值的场景 = **复刻论文的定制注意力内核**：
  - 论文结论：移动 GPU 片上缓存带宽受限，KV cache 直接从共享内存**流式读取** + 在线 softmax
    比缓存还快；用 8 宽 FP16 向量吃满 Adreno 的 FP16 双倍速率。
  - 这只有在**大上下文 prefill**（8k~32k token）才明显；我们 demo 的 prompt 只有几十 token，没收益。
- 决策：**M5 作为可选进阶**，单独写一个 OpenCL prefill 内核和标准后端做对照实验，否则不做。

### 2.3 NPU 怎么调用？两条路

| 路线 | 方式 | 工作量 | 可行性 |
|---|---|---|---|
| **A. llama.cpp NNAPI 后端**（推荐先试） | `-DGGML_NNAPI=ON` → Android NNAPI → 高通 HTP 驱动 | 低（复用现有代码） | op 支持有限，部分层会回退 CPU；小模型（0.6B/1B）效果最好 |
| **B. QNN SDK（AI Engine Direct）** | ONNX→QNN 模型转换 + QNN runtime，脱离 llama.cpp | 高（整个运行时重写） | 完全掌控 HTP，但相当于新项目 |

- 备注：`ro.nnapi.extensions.deny_on_product=true`（小米限制部分 NNAPI 扩展），
  且 NNAPI 是否真的路由到 HTP 取决于高通驱动注册，需实测日志确认。
- 论文未实现 NPU（Discussion 里只提到"prefill 可以跑在 NPU 上"作为扩展方向）。

---

## 3. GPU / NPU 监测方案

### 3.1 免 root 能拿到的（M2 立刻可做）

1. **GPU 内存快照**：`dumpsys gpu`（新发现，实测可读）
   - 输出含 `Memory snapshot for GPU 0: Global total: ...` 与分进程 `Proc <pid> total: ...`
   - 采样方式：推理期间周期性 `adb shell dumpsys gpu`，按 llama 进程 PID 归属内存用量
2. **CPU 三簇频率/温度/负载**：沿用 demo2 采样器
3. **系统级能耗估计**：`dumpsys batterystats` 按 UID 的能耗（mAh，粗粒度，用于横向对比）
4. **Vulkan 设备信息**：程序内 `vkGetPhysicalDeviceProperties` 读 GPU 名称/版本（一次性）

### 3.2 root 才能拿到的（M2-root / M3）

1. **GPU 频率 + 占用**：`/sys/class/kgsl/kgsl-3d0/gpuclk`（Hz）+
   `gpu_busy_percentage`（%）→ 并入现有采样器 root 分支，与 CPU 频率并行采样
2. **GPU 内存**：`gpumem_mapped_bytes`（KGSL debugfs，论文口径）
3. **真实功耗**：`power_now(uW) × voltage_now(uV) → W`（论文口径），沿用 demo2 的 root 分支已有骨架
4. **锁 GPU 频率**：`max_gpuclk` → 对照"锁 CPU 频率"实验（demo2 已有锁 CPU 的 resolve_freq 逻辑可复用）

### 3.3 NPU 监测：没有直接遥测，用替代指标

- 高通 HTP **不暴露任何用户空间可读的频率/负载**（已全盘搜索确认）。
- 替代方案（间接判断 NPU 是否参与）：
  - **耗时差**：同一模型 NNAPI 后端 vs CPU/Vulkan 后端的 TTFT/TPOT 对比
  - **CPU 占用下降**：NPU 卸载后 CPU busy% 应显著下降（现有采样器直接可测）
  - **功耗差**：root 后对比三种后端的功耗曲线

---

## 4. 分阶段实施计划（里程碑）

| 阶段 | 内容 | 前置 | 产出 |
|---|---|---|---|
| **M0** | CPU 基线（XNNPACK） | ✅ 已完成 | demo2 结果：~8 tok/s |
| **M1** | Vulkan 版 llama.cpp 交叉编译 + 推送 + `-ngl` 卸载 + 速度对比 | NDK 工具链 | GPU 推理跑通 + 速度提升数据 |
| **M2** | 监测扩展：免 root 加 `dumpsys gpu` 内存采样；root 加 gpuclk/gpu_busy/功耗 | root 可选 | GPU 与 CPU 同窗口的芯片状态曲线 |
| **M3** | 锁 GPU 频率实验（root），对照锁 CPU | root | "锁频 × 推理" 的完整对比 |
| **M4** | NNAPI 后端 NPU 实验（小模型 0.6B/1B 优先） | M1 编译骨架 | NPU 参与的间接证据（耗时/CPU 占用） |
| **M5** | （可选）OpenCL 定制 prefill 内核，复刻论文方法 | M2 | 大上下文下的内核收益对照 |

> 无 root 也能走到 M1 + M2 的免 root 部分；M2-root/M3 需要一台已 root 的骁龙机
> （或给这台小米 13 Pro 解 BL 锁 root，可另行评估）。

---

## 5. 需要准备的资源

1. 小模型：Qwen3-0.6B / 1.7B（NNAPI/NPU 实验用，GPU 全量卸载也更快）
2. NDK + Vulkan 工具链（交叉编译 `GGML_VULKAN`，需 shaderc/glslang 或 LLAMA_VULKAN 自带）
3. （可选）root 手机一台
4. 真机复核项：NNAPI 是否真的路由到 HTP（跑 M4 时看 llama 日志的 backend 输出）

## 6. 风险与注意

- **4B 全量 GPU 卸载内存**：12GB 够，但注意 GPU 内存快照确认没有走 mmap 回退到 CPU。
- **Vulkan 编译较重**：比 CPU 版多一步 SPIR-V 着色器工具链，首次构建要耐心。
- **NNAPI 后端回退**：部分算子不支持会静默回退 CPU，必须看日志确认真实卸载层数。
- **论文设备是 Adreno 830/750**（S25+/OnePlus 12），我们是 Adreno 740——结论趋势可比，绝对值不同。
- 免 root 下 GPU 功耗拿不到，先靠「速率 + 温度 + 能耗代理」间接评价 GPU 收益。

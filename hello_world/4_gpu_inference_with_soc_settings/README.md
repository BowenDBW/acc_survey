# 4_gpu_inference_with_soc_settings — GPU 推理 + SoC 遥测

> 承接 `3_gpu_inference_demo`（Vulkan/OpenCL GPU 推理）。本 demo 回答：
> **无 root 下 GPU 的频率与占用能不能读？**
> 结论一句话：**GPU 只有温度（thermal zone）与限频等级（cooling device）可读；
> 实时频率 gpuclk / 占用 gpu_busy_percentage 被 MIUI 的 sysfs DAC 拒绝，
> 且 SM8550 的 5.15 内核已把这两项从 kgsl ioctl 里移除——无 root 不可读。**

- 设备：小米 13 Pro (2210132C/nuwa) · SM8550 (Adreno 740) · **无 root** · shell (uid 2000) · Android 16
- 日期：2026-09-05

---

## 一、实测结论

| 指标 | 可读 | 路径 / 实测结果 |
|---|---|---|
| GPU 温度 | ✅ | `/sys/class/thermal/thermal_zone63..70` = `gpuss-0..7`，空闲 29~30°C |
| GPU 限频等级 | ✅ | `/sys/class/thermal/cooling_device37` (gpu)，空闲 cur=2 / max=7 |
| GPU 固件/版本 | ✅ | `/dev/kgsl-3d0` ioctl `GETPROPERTY` type=0x29/0x2A（返回 "Adre"、GPU 版本）|
| **GPU 频率 gpuclk** | ❌ | `/sys/class/kgsl/kgsl-3d0/gpuclk` → **Permission denied** |
| **GPU 占用 %** | ❌ | `gpu_busy_percentage` → Permission denied |
| **gpubusy 计数器** | ⚠️ | 文件能读但返回 `0 0`；写入使能（`echo 1 > gpubusy`）被拒 → 计数无效 |
| **devfreq cur_freq** | ❌ | `/sys/class/devfreq/*/cur_freq` → Permission denied |
| debugfs | ❌ | 未挂载 |

## 二、关键证据：kgsl ioctl 也不给（`kgsl_probe.c`）

`/dev/kgsl-3d0` 是 **0666 人人可开**。用 NDK r26c 交叉编译 `kgsl_probe.c`，
对设备节点发 `IOCTL_KGSL_DEVICE_GETPROPERTY`（`_IOWR(0x2A, 0x2, struct kgsl_device_getproperty)`，
64 位下 struct 24 字节，ioctl 号里编码的 size 必须为 24），暴力试 property type
0x1..0x100：

- 通道通：shell 能 open + 能拿设备/固件信息；
- **0x1..0x100 全范围无 GPU_CLOCK / GPU_BUSY_PERCENT / GPU_TEMP**（返回 ENODEV）。
- 判读：SM8550 (kernel 5.15) 把动态频率/占用属性从 UAPI ioctl 移除，仅 sysfs 保留
  ——而 sysfs 对 shell 是 DAC 拒绝。**不是没找到接口，是内核不给非特权。**

复现：
```bash
NDK=.../android-ndk-r26c/toolchains/llvm/prebuilt/windows-x86_64
$NDK/bin/aarch64-linux-android24-clang.cmd kgsl_probe.c -o kgsl_probe
adb push kgsl_probe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/kgsl_probe
adb shell /data/local/tmp/kgsl_probe
```

## 三、无 root 可用的 GPU 观测手段（真负载验证 ✅）

- **温度**：采样 `gpuss-0..7` 最大温度 → 负载代理；
- **限频等级**：`cooling_device37` cur_state → 是否被热限频；
- **GPU 静态上限频率**：OpenCL `CL_DEVICE_MAX_CLOCK_FREQUENCY`（Adreno 740 ~719MHz，只给上限）。
- 真负载观察：并行跑 `3_gpu_inference_demo` 的推理 + `soc_sampler_remote.sh`。

### 实测（2026-09-05）

并行跑 HTP 推理（与 GPU 同 die 同子系统，SM8550 上 HTP 归在 gpuss 热域）+ 采样器，
18 样本全出，**温度响应明显**：

| 指标 | 空闲 | 负载 | 变化 |
|---|---|---|---|
| GPU 温度 (gpuss max) | 27°C | **43~49°C** | +16~22°C |
| NPU 温度 (nspss max) | 26°C | **43~45°C** | +17~19°C |
| CPU 温度 (大核 max) | 27~31°C | **54~69°C** | +25~40°C |
| CPU 占用 | 6~8% | **46~68%** | +40~60pp |
| 大核频率 | ~2.0GHz | c3~c6 钉 2592MHz | 满频 |
| 限频等级 | gpu 2/7 · cdsp 0/7 · cdsp_hw 0/1 | 不变 | 未触发热限频 |

原始数据存 `live_gpu_telemetry.log`（= 6_ 的 `live_htp_telemetry.log` 副本）。
**结论：读不到 GPU 时钟，但温度+频率+占用在真实计算负载下响应明显，
"温度/限频/CPU 响应"就是无 root 下最实用的 GPU 负载观测手段。**

> GPU 频率要"真读"只剩两条路：① root（解锁 MIUI，直接 cat gpuclk）；
> ② 等内核/驱动放开。app 层无解。

## 四、本目录工具

```
4_gpu_inference_with_soc_settings/
├── README.md               ← 本文
├── kgsl_probe.c            ← GPU ioctl 探测源码（证据，可复现）
├── soc_sampler_remote.sh   ← 手机端遥测采样（CPU/GPU/NPU 温度、限频等级、CPU 频率/占用）
│                             （与 6_npu_inference_with_soc_settings 共用同一份）
└── live_gpu_telemetry.log  ← 真负载闭环验证原始数据（负载期间 18 样本，= 6_ live_htp 副本）
```

采样器用法见 `6_npu_inference_with_soc_settings/README.md` 第五节。

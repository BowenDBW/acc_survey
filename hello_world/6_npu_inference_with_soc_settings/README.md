# 6_npu_inference_with_soc_settings — NPU 推理 + SoC 遥测

> 承接 `5_npu_inference`（NPU 推理已跑通）。本 demo 回答：
> **无 root 下能不能读到 GPU / NPU 的频率与占用？**
> 结论一句话：**CPU 频率/占用/温度全可读；GPU 只有温度与限频等级可读，
> 实时频率/占用被内核封死；NPU 频率无 sysfs，理论可借 HAP/QNN 通道读，但缺工具链。**

- 设备：小米 13 Pro (2210132C/nuwa) · SM8550 · HTP v73 · **无 root** · shell (uid 2000) · Android 16
- 日期：2026-09-05
- 前置：`5_npu_inference` 的 SDK 已部署在 `/data/local/tmp/llama.cpp`（见其 README）

---

## 一、实测：哪些能读，哪些不能（全部 shell 直测）

| 指标 | 可读 | 路径 / 结果 |
|---|---|---|
| CPU 频率 | ✅ | `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`（实测 2.016~2.592 GHz）|
| CPU 占用 | ✅ | `/proc/stat` 两次采样差值 |
| CPU 温度 | ✅ | `/sys/class/thermal/thermal_zone*` |
| GPU 温度 | ✅ | `thermal_zone63..70` = `gpuss-0..7`（29~30°C 空闲）|
| GPU 限频等级 | ✅ | `cooling_device37` (gpu cur/max, 空闲 cur=2/max=7) |
| **GPU 频率 gpuclk** | ❌ | `/sys/class/kgsl/kgsl-3d0/*` → **Permission denied**（MIUI DAC）|
| **GPU busy%** | ❌ | 同上；`gpubusy` 能读但显示 `0 0`，写入使能被拒 → 计数无效 |
| **devfreq cur_freq** | ❌ | `/sys/class/devfreq/*/cur_freq` → Permission denied |
| NPU/CDSP 限频 | ✅ | `cooling_device38` (cdsp_hw) / `cooling_device39` (cdsp) cur/max（空闲 0/1、0/7）|
| **NPU/HTP 频率** | ❌ | **无任何 sysfs 节点**（`/sys/.../cdsp*`、`/sys/module/adsprpc` 均不存在）|
| 电池功率 | ❌ | `power_now/current_now/voltage_now` → Permission denied |
| GPU/NPU 时钟调试节点 | ❌ | debugfs 未挂载（`/sys/kernel/debug` 不存在）|

## 二、GPU 为什么读不到频率——ioctl 也不给（本 demo 的关键实测）

`/dev/kgsl-3d0` 是 **0666（人人可开）**，kgsl 有 `GETPROPERTY` ioctl。写了个
`kgsl_probe.c`（NDK r26c 交叉编译，见本目录/4_）暴力试 ioctl nr=0x2、property
type 0x1..0x100：

- ioctl 通道本身 **shell 能开、能调通**，能读到设备信息/固件版本
  （type=0x29 返回 `"Adre"`、type=0x2A 返回 GPU 版本号等）；
- 但 **整个 0x1..0x100 范围内没有 GPU_CLOCK / GPU_BUSY_PERCENT / GPU_TEMP**。
- 结论：SM8550 的 5.15 内核把"实时时钟/占用"属性**从 UAPI ioctl 里删掉了**，
  只剩 sysfs——而 sysfs 对 shell 是 DAC 拒绝。**GPU 实时频率/占用 = 无 root 不可读。**

> 与 `2_inference_with_soc_settings` README 里"GPU 频率/功耗需 root"的旧说法一致，
> 本 demo 用 ioctl 层验证了它：不是没找到接口，是内核压根不向非特权暴露。

## 三、NPU 频率的三条理论通道（都差一步工具链，未闭环）

| 通道 | 原理 | 卡在哪 |
|---|---|---|
| ① HAP_power_get_clk_Freq | Hexagon SDK `HAP_power.h` 有 `HAP_power_get(ctx,&resp)`，`resp.type=HAP_power_get_clk_Freq` 直接返回 DSP/HTP 核心时钟 Hz；`HAP_power_get_hcp_core_clk_Freq` 同理。已从 tinygrad vendored 的 Hexagon SDK 头确认接口存在（`extra/dsp/include/HAP_power.h`）| `HAP_power_set` 是 **DSP 侧**符号（已 nm 实测：libggml-htp-v73.so 里是 `U HAP_power_set`，由 CDSP 运行时提供）。要读就要**自定义 skel 跑进 DSP**，需 Hexagon 工具链（snapdragon-toolchain Docker 镜像，本机无 Docker）|
| ② QNN perfinfra getPerfcount | QNN 2.29+ 的 `QnnHtpDevice_PerfInfrastructure_t` vtable 有 `getPerfcount`，返回 `dcvsfreq`(Hz)、busy/totalCycles(→占用%)、activeDcvsMode、throttledCycles。走 fastrpc，与 llama.cpp 同通道 | 手头 QNN 库（ShadowNPU/libs_bin，SNPE/qaisw 2023 版）**vtable 只有 4 个槽**（createPowerConfigId/destroy/setPowerConfig/setMemoryConfig），无 getPerfcount；新版 QNN 库 + 头文件公开渠道拿不到（已下载的 `qnn_include/` 是 2023 版头，仅够 bootstrap）|
| ③ HTP 自身 PMU | llama.cpp skel 有 `profiler()` 远程方法 + `hex_get_pmu()`（HVX PMU 计数）| 只有**周期计数**不是真实频率；且要改 skel 也要工具链 |

**方向判断**：这条"SDK 放普通文件夹"的思路证明了**执行**（fastrpc 通道 shell 可通，
推理能跑）✅，但**频率回读**是另一道权限边界——内核/CDSP 只把频率交给
① 自定义 in-DSP 代码（skel）或 ② 新版 SDK 的性能基础设施。两者都需要把
Hexagon 工具链 / 新版 QNN 库弄到本机，是下一阶段的事。

## 四、无 root 现在就能用的 NPU 观测手段（真负载验证 ✅）

LLM/NPU 场景最实用的"占用"是**吞吐(tokens/s)+ 温度 + 限频等级**：

- 吞吐：`llama-bench`/`llama-simple`（见 `5_npu_inference/PERF_NOTES.md`，pp512=44.1 t/s）
- 温度/限频：推理期间并行跑 `soc_sampler_remote.sh` 可观察到温度抬升、
  频率爬升 → **NPU 在干活的间接证据**。

### 实测：HTP 推理期间并行采样（2026-09-05，已完成闭环验证）

方法：`adb shell llama-simple ... -ngl 99`（host 级后台）+ `soc_sampler_remote.sh 18 1`（前台），
即"一个 adb 跑推理、另一个 adb 跑采样"两个独立 host 进程（**不要**在同一个 adb 会话里 `setsid &`，
那个会挂住 adb 会话；host 级 `run_in_background` 才行）。

| 指标 | 空闲基线 | HTP 推理期间 | 变化 |
|---|---|---|---|
| GPU 温度 (gpuss max) | 27°C | **43~49°C** | +16~22°C |
| NPU 温度 (nspss max) | 26°C | **43~45°C** | +17~19°C |
| CPU 温度 (大核 max) | 27~31°C | **54~69°C** | +25~40°C |
| CPU 占用 | 6~8% | **46~68%** | +40~60pp |
| 大核频率 | ~2.0 GHz 闲逛 | **c3~c6 钉 2592 MHz** | 满频 |
| 限频等级 | gpu 2/7 · cdsp 0/7 · cdsp_hw 0/1 | 不变 | 未触发热限频 |

原始数据存于本目录 `live_htp_telemetry.log`（含 llama 日志确认 `HTP0 compute buffer` 已分配，
`resolve_fused_ops`/`sched_reserve: HTP0` 证明推理跑在 NPU 上）。
**结论：即使读不到 NPU 时钟，温度+频率+占用这三条可读通道在真实 HTP 负载下响应明显，
完全可以作为"NPU 是否在干活/负载多大"的观测手段。**

## 五、本目录工具

```
6_npu_inference_with_soc_settings/
├── README.md               ← 本文
├── soc_sampler_remote.sh   ← 手机端遥测采样（CPU freq/util、GPU/NPU 温度与限频等级）
├── live_htp_telemetry.log  ← 真负载闭环验证原始数据（HTP 推理期间 18 样本 + llama 日志）
├── kgsl_probe              ← GPU ioctl 探测二进制（aarch64, NDK 编译）
├── qnn_include/            ← 已拉取的 QNN SDK 头（2023 版，QnnInterface/HtpDevice/PerfInfra）
│                            （来源: XiaoMi/StableDiffusionOnDevice qnn_8550, 供后续 perfinfra 用）
├── libcdsprpc.so           ← 设备 /vendor/lib64 拉出，已证实无 HAP_power_get
├── libggml-htp-v73.so      ← 设备上 skel，已证实 U HAP_power_set（DSP 侧）
└── libggml-hexagon.so      ← 设备上 host 后端
```

采样器用法：
```bash
adb push soc_sampler_remote.sh /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/soc_sampler_remote.sh
adb shell sh /data/local/tmp/soc_sampler_remote.sh <count> <interval_s>
# 推理期间并行：另开一个终端跑 5_npu_inference 的 llama-simple/llama-bench 即可
```

## 六、下一步（接续点）

1. **闭环 ①**：装 Hexagon 工具链（或恢复 snapdragon-toolchain Docker），给 llama.cpp
   的 `htp_iface.idl` 加一个 `get_clk` 远程方法（内部调 `HAP_power_get_clk_Freq`），重编 skel，
   host 侧 dlsym 调用 → **NPU 实时频率直接可读**。
2. **闭环 ②**：拿到 QNN ≥2.29 的 libQnnHtpV73(+Stub/Skel)，用 `qnn_include/` + 补
   getPerfcount 头，写 perfinfra 轮询工具 → dcvsfreq + busy%。
3. ✅（已完成 2026-09-05）采样器在 adb 会话里被后台进程拖住的坑已解决：
   **host 级双后台**（推理 `run_in_background` 单独起 adb、采样前台再起一个 adb），
   实测 18 样本全出（见第四节）。`setsid &` 在 MIUI adbd 下会挂住会话，别再用了。
4. GPU 频率若一定要读：只剩 root（MIUI 解锁）或换一条已带 AOP/firmware 权限的路径。

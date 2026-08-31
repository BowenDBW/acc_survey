# 2_inference_with_soc_settings — 推理 + 芯片频率/温度/负载采集

> 目标：在推理（TTFT / TPOT 测量）的同时，用手机端 `soc_sampler.sh` 并行采样
> **CPU 三簇频率、温度、CPU 负载**（均免 root 可读）；若手机已 root，
> 还可额外采集 **电池瞬时功率 (power_now) 与 GPU 频率/占用**，并支持锁频。
>
> 本目录是 `1_inference_demo` 的进阶：demo1 只测推理速度，本 demo 把"跑得多快"
> 和"芯片在什么状态（频率/温度/负载）"关联起来。

## 环境（与 demo0/1 一致）

| 项 | 值 |
|---|---|
| 手机 | 小米 13 Pro (2210132C / nuwa), Android 16, **骁龙 8 Gen 2 (SM8550)**, 12GB RAM |
| 开发机 | Windows 11 (Git Bash) |
| adb | `D:\code_env\android_sdk\platform-tools\adb.exe` (USB 调试已开) |
| python | miniconda 的 `python`（`common.sh` 自动规避 Windows Store 的 `python3` 占位符） |
| 模型/二进制 | `../0_environment_setup/models` + `binaries`（本 demo 与其共用，见其 README） |

> 模型、二进制、交叉编译、推送全部复用 `0_environment_setup`（demo0），
> 本目录只保留 **采样器 + 运行编排 + 汇总分析**，不重复环境搭建。

## 目录结构

```
2_inference_with_soc_settings/
├── config.json                 # ★ 所有可调参数（推理/锁频/采样/功耗模式）
├── scripts/
│   ├── 04_run_with_soc_monitor.sh   # ★ 主脚本: 空闲基线 + 推理 + 并行采样 + 汇总
│   ├── soc_sampler.sh               # 手机端采样循环 (POSIX sh, 推送到 /data/local/tmp 运行)
│   └── 05_plot_results.py           # (可选) 把某次 run 画成图, 需要 matplotlib
└── results/
    ├── soc_results.csv          # 所有 run 的历史汇总（每跑一行追加）
    └── qwen3-4b-default_*/      # 每次运行: inference.log / soc_idle.csv / soc_samples.csv / summary.json
```

## 快速上手

```bash
# 唯一入口：全部参数都在 config.json 里调，脚本不硬编码
bash scripts/04_run_with_soc_monitor.sh
```

脚本流程（打印 [1/5]~[5/5] 分步）：

1. 解析 `config.json`，探测 root / 功耗 / GPU 读数能力；
2. (root 才生效) 按配置锁 CPU 三簇频率 + GPU 频率，结束后自动还原；
3. 推理前先采一段**空闲基线**（warmup + measure）；
4. `am kill-all` 释放后台内存后，启动采样器并跑推理，**推理期间并行采样**；
5. 汇总分析：输出 `summary.json` 并追加一行到 `results/soc_results.csv`。

> 每次 run 生成独立目录 `results/<label>_<时间戳>/`，包含 4 个文件：
> `inference.log`（llama 原始日志）、`soc_idle.csv`（空闲基线）、
> `soc_samples.csv`（推理期间采样，含 `MARK_INFERENCE_START/END` 标记）、
> `summary.json`（结构化汇总）。历史横向对比在 `soc_results.csv`。

## 命令行覆盖参数（不改 config.json 跑不同设定）

脚本支持在**运行时**用命令行覆盖任意参数；**没指定的项自动走 `config.json`
默认值**。这样不用反复改配置文件，就能连续跑几个不同设定，用
`soc_results.csv` 横向对比各设定下的**速率 / 功耗 / 温度**。

```bash
bash scripts/04_run_with_soc_monitor.sh [选项...]
```

| 选项 | 值 | 对应 config.json 键 |
|---|---|---|
| `--label NAME` | run 目录名前缀 | `run.label` |
| `--mode simple\|bench` | 推理工具 | `run.mode` |
| `--model FILE` | 模型文件名 | `run.model` |
| `--prompt "TEXT"` | 生成提示词 | `run.prompt` |
| `--n-gen N` | 生成 token 数（simple） | `run.n_gen` |
| `--n-ctx N` | 上下文长度 | `run.n_ctx` |
| `--threads N` | CPU 线程数 | `run.threads` |
| `--pin-cpu02 V` | 锁小核簇 cpu0-2（格式见下节） | `soc.pin_cpu_freqs.cpu0-2` |
| `--pin-cpu36 V` | 锁中核簇 cpu3-6 | `soc.pin_cpu_freqs.cpu3-6` |
| `--pin-cpu7 V` | 锁大核簇 cpu7 | `soc.pin_cpu_freqs.cpu7` |
| `--pin-gpu V` | 锁 GPU | `soc.pin_gpu_freq` |
| `--interval-ms N` | 采样周期 ms | `sampling.interval_ms` |
| `--idle-warmup N` | 空闲基线预热 s | `sampling.idle_warmup_s` |
| `--idle-measure N` | 空闲基线测量 s | `sampling.idle_measure_s` |
| `--power auto\|always` | 功耗模式 | `power.mode` |
| `--help`, `-h` | 显示帮助并退出 | — |

锁频值与 config.json 完全同格式（`0` / `max` / `min` / 档位 `1..N` / MHz/GHz，
自动读表换算并吸附到最近档位，见下一节）。

**演示多设定对比**（每行一条命令，跑完看 `results/soc_results.csv` 的行差）：

```bash
# 三连跑：默认线程 → 少线程 → 大核锁最高(需 root 才真正锁上)
bash scripts/04_run_with_soc_monitor.sh --label cmp-default            # 用 config.json 默认
bash scripts/04_run_with_soc_monitor.sh --label cmp-t4    --threads 4 --n-gen 64
bash scripts/04_run_with_soc_monitor.sh --label cmp-bigmax --pin-cpu7 max --n-gen 64

# 换模型 / 换生成长度也行
bash scripts/04_run_with_soc_monitor.sh --label cmp-n256 --n-gen 256
```

> 免 root 时 `--pin-*` 会正常解析并警告"需 root 锁频"，只跳过写入那一步，
> 其余照常跑——所以无 root 也能用这个入口做 n_gen/threads/模型 的对比。

## config.json 说明

| 键 | 示例值 | 含义 |
|---|---|---|
| `run.label` | `qwen3-4b-default` | 本次运行目录名前缀 |
| `run.mode` | `simple` / `bench` | `simple`=llama-simple 真实生成；`bench`=llama-bench 多次平均 |
| `run.model` | `Qwen3-4B-Q4_K_M.gguf` | 模型文件名（在共享 models 目录） |
| `run.prompt` | `What is the capital...` | 生成提示词（建议英文避免转义） |
| `run.n_gen` | `64` | 生成 token 数（simple 模式） |
| `run.n_ctx` | `4096` | 上下文长度 |
| `run.threads` | `8` | CPU 线程数 |
| `soc.pin_cpu_freqs` | `cpu0-2:0` 等 | root 锁频目标，0=不锁。**不用手写 kHz**，支持：`max`/`min`=最高/最低档、`1..N`=第几档（1=最低档）、或直接给频率（`2016`=2016MHz、`2.0G`=2GHz，自动吸附到最近可用档位） |
| `soc.pin_gpu_freq` | `0` | root 锁 GPU 频率，同上格式（GPU 档位表需 root 才能读，见下） |
| `soc.governor_after` | `schedutil` | 锁频结束后还原的 governor |
| `sampling.interval_ms` | `100` | 采样周期 |
| `sampling.idle_warmup_s` | `5` | 空闲基线预热时长 |
| `sampling.idle_measure_s` | `5` | 空闲基线测量时长 |
| `power.mode` | `auto` / `always` | `always` 强制要求读到功耗否则报错退出 |

### 骁龙 8 Gen 2 (SM8550) 频率取值范围（本机实测 2026-08-30）

**你不需要手写 kHz**。脚本会自动去手机读可用频率表，把你填的
`max`/`min`/挡位序号/MHz/GHz 换算成内核认的 kHz 写入。各簇实际有多少档、每档多少，
以真机为准（可用下方命令随时复核）：

```bash
# CPU（免 root 可读）
adb shell cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies   # 小核簇 A510
adb shell cat /sys/devices/system/cpu/cpu3/cpufreq/scaling_available_frequencies   # 中核簇 A715
adb shell cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_available_frequencies   # 大核簇 X3
# GPU（需 root）
adb shell su -c 'cat /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies'
```

| 簇 | 核心 | 档位数 | 最低档 | 最高档 | 本机实测全部档位 (kHz) |
|---|---|---|---|---|---|
| 小核簇 (little) | cpu0-2 (A510) | 16 | 307.2 MHz | 2.016 GHz | `307200 441600 556800 672000 787200 902400 1017600 1113600 1228800 1344000 1459200 1555200 1670400 1785600 1900800 2016000` |
| 中核簇 (mid) | cpu3-6 (A715) | 19 | 499.2 MHz | 2.707 GHz | `499200 614400 729600 844800 940800 1056000 1171200 1286400 1401600 1536000 1651200 1785600 1920000 2054400 2188800 2323200 2457600 2592000 2707200` |
| 大核簇 (big) | cpu7 (X3) | 20 | 595.2 MHz | 2.957 GHz | `595200 729600 864000 998400 1132800 1248000 1363200 1478400 1593600 1708800 1843200 1977600 2092800 2227200 2342400 2476800 2592000 2726400 2841600 2956800` |

要点：

- **输入格式**（`pin_cpu_freqs` / `pin_gpu_freq` 的每个值）：
  - `0` = 不锁该簇
  - `max` / `min` = 该簇最高 / 最低档
  - `1..N` = 第几档（**1 = 最低档**，`N` = 该簇档位数，越界会警告并跳过）
  - 直接给频率：`2016`（整数 >100 视为 MHz）、`2016.5M`、`2.0G`、`2.0`（裸小数视为 GHz）——
    都会**自动吸附到最近的可用档位**
- 三簇都是 **0.3~3.0 GHz 级别**，步进约 100~150 MHz，各簇档位数和最小值不同
  （小 16 / 中 19 / 大 20 档；最低 307/499/595 MHz）。
- 小米 13 Pro 出厂限制了大核最高到 **2956.8 MHz**（非 8 Gen 2 理论 3.2 GHz），
  用 `max` 就是这一档。
- GPU（Adreno 740）档位表需 root 读取；本机非 root 无法实测，root 后先 `su -c cat ...`
  确认再填（骁龙 8 Gen 2 的 Adreno 740 典型区间约 0.3~0.7 GHz，但以你手机实际列表为准）。
- 想锁"某一档"最省事：直接写 `max` 或 `min`，或抄上面的挡位序号（如小核锁第 8 档写 `8`）。

## 测量口径

| 指标 | 定义 | 算法 |
|---|---|---|
| **TTFT** | 请求发出到首个输出 token | ≈ `prompt eval time`(prefill) + 1 个 decode 步 |
| **TPOT** | 每个输出 token 的解码耗时 | = `eval time / runs` (llama-simple) 或 `1000/tg_tps` (llama-bench) |
| CPU 三簇频率 | little(A510)/mid(A715)/big(X3) 代表核 | 采样窗口内平均值/峰值/频率分布 |
| 温度 | cpuss-0 与 battery | 窗口内平均/峰值/温升 |
| CPU 负载 | /proc/stat 差分 | 窗口内平均/峰值 busy% |
| 能耗代理 | Σ(3·f0+4·f3+1·f7)·Δt (kHz·s) | 免 root 的相对能耗估算（按簇核数加权，用实测 Δt 积分） |
| 功耗 | battery `power_now` (uW) | **仅 root**：平均/峰值/能量(J)/空闲基线/净功耗 |

> 采样窗口 = `[MARK_INFERENCE_START, MARK_INFERENCE_END]`，只统计推理期间的芯片状态；
> 空闲基线单独采一段做对比基准。相邻样本用**实测时间差**积分，更准确。

## 实测结果 (小米13 Pro, 2026-08-30, 骁龙 8 Gen 2, 免 root)

> 条件：CPU/XNNPACK (`-ngl 0`)，`n_gen=64`，`threads=8`，`interval=100ms`。
> 免 root：未锁频（调度器自动调频），无功耗/GPU 读数。

| 运行 | 窗口 | 采样点 | **TTFT** | **TPOT** | 生成速度 | little avg | mid avg | big avg | CPU busy avg | CPU 峰值温度 |
|---|---|---|---|---|---|---|---|---|---|---|
| 205237 | 12.9 s | 57 | 2713 ms | 146 ms | 6.8 tok/s | 2004 MHz | 2480 MHz | 3012 MHz | 72.5% | 86.0 °C |
| 211542 | 11.7 s | 49 | 2414 ms | 134 ms | 7.5 tok/s | 1713 MHz | 2391 MHz | 3187 MHz | 56.3% | 78.4 °C |

要点：

- **TPOT ≈ 130~150 ms/token**（4B Q4_K_M，与 demo1 的 141ms 互相印证），波动 ~10%，
  受后台负载与温度影响。
- **推理期间大核基本满频**（3187 MHz，211542 次占 100%），温度冲到 78~86 °C 后触发降频，
  中/小核频率随调度器波动。
- 温度起点不同（空闲基线 28~35 °C）会显著影响窗口内峰值与降频行为——两次 run 差异主要来自此。
- 免 root 的**能耗代理**可横向对比不同配置的相对能耗；真实功率/GPU 需 root 后补测。

完整明细：`results/soc_results.csv`（历史汇总），`results/qwen3-4b-default_*/summary.json`（单次明细）。

### 命令行覆盖参数对比实验（2026-08-31，演示用）

> 用命令行覆盖参数连跑 4 组，全程免 root（未锁频），验证"不同设定 → 不同速率/功耗/温度"：
> 线程数 8/4/2 各一组（`n_gen=64`），外加一组长生成 `n_gen=256`（`threads=8`）。
> 4 组背靠背连跑，手机已处于温热状态（早前 205237/211542 是从冷启动跑的）。

| 运行 | 线程 | n_gen | 采样点 | **TTFT** | **TPOT** | 生成速度 | big avg | CPU busy | 峰值温度 | 温升 ΔT | 能耗代理 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cmp-t8 | 8 | 64 | 45 | 2320 ms | 124.3 ms | **8.04 tok/s** | 3187 MHz | 57.4% | **76.0 °C** | 41.0 °C | 184.4 M |
| cmp-t4 | 4 | 64 | 47 | 2341 ms | 128.9 ms | 7.76 tok/s | 3187 MHz | 56.8% | 76.8 °C | 33.0 °C | 192.3 M |
| cmp-t2 | 2 | 64 | 47 | 2371 ms | 128.2 ms | 7.80 tok/s | 3187 MHz | 57.2% | 77.6 °C | 34.6 °C | 189.9 M |
| cmp-t8-n256 | 8 | **256** | 54 | 2337 ms | 129.6 ms | 7.72 tok/s | 3187 MHz | 57.0% | 77.2 °C | 35.4 °C | **231.1 M** |

要点：

- **线程数对 4B 模型几乎无影响**：`threads=8/4/2` 的 TPOT 都在 124~129 ms，差异 <5%，
  与 run 之间的随机波动同级；t8 略优。原因是解码瓶颈在**单大核单线程**，
  4B Q4 模型太小、多线程并行帮不上忙——换成 8B 或更大模型线程数才会体现差异。
- **大核推理期间 100% 时间满频 3187 MHz**（大核簇 avg=peak=3187，分布占比 1.0），
  中/小核随调度器波动——这是免 root 下"调度器自动调频"的典型形态。
- **能耗代理能反映运行时长**：同参数下 `n_gen=256` 与 `n_gen=64` 速率相当，
  但能耗代理 +25%（231.1 vs 184.4），即"跑得越久耗能越多"；横向对比不同设定时
  应尽量用相同的 `n_gen`，否则先把能耗代理归一化。
- **温度**：连跑时手机已温热，峰值 76~78 °C、温升 33~41 °C（冷启动那两组是 78~86 °C）；
  从冷启动单次温升可到 ~41 °C。做对比实验建议每次跑前等温度回落到同一水平。

> 复现这些结果只需三行命令（无 root 也能跑）：
> ```bash
> bash scripts/04_run_with_soc_monitor.sh --label cmp-t8 --threads 8 --n-gen 64
> bash scripts/04_run_with_soc_monitor.sh --label cmp-t4 --threads 4 --n-gen 64
> bash scripts/04_run_with_soc_monitor.sh --label cmp-t2 --threads 2 --n-gen 64
> ```

## 注意事项

1. **免 root 即可跑**：频率/温度/负载均从 `/sys` 可读，采样器以普通 shell 运行。
   但锁频、功耗、GPU 读数**需要 root**；`power.mode=always` 且无 root 时会报错退出。
2. **锁频风险**：root 锁频会写 `scaling_max/min_freq`，脚本跑完自动还原原值并恢复 governor；
   若 adb 中途断开可能来不及还原，建议锁频实验后手动确认。
3. **温度影响**：连续多次 run 后手机发热，结果会漂移。对比实验建议每次跑前等温度回落，
   或直接用本 demo 的温度字段把热态/冷态分开分析。
4. **手机内存**：Qwen3-8B 约 4.7GB，12GB 机型建议跑前清理后台（脚本已自动 `am kill-all`），
   8B 加载 OOM 时把 `n_ctx` 调小或改用 4B。
5. **（可选）画图**：`scripts/05_plot_results.py <run_dir>` 可把某次 run 的采样画成图
   （需 `pip install matplotlib`）。本 README 以表格呈现数据，不依赖图片。

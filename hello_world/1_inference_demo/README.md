# 1_inference_demo — 小米13 Pro 端侧跑 Qwen3 (llama.cpp) + TTFT/TPOT 测量

> 目标：在 USB 连接的小米 13 Pro 上，用交叉编译的 llama.cpp 跑 Qwen3-8B / Qwen3-4B，
> 并测量 **TTFT (Time To First Token)** 与 **TPOT (Time Per Output Token)**。

## 环境（本机实测）

| 项 | 值 |
|---|---|
| 手机 | 小米 13 Pro (2210132C / nuwa), Android 16, **骁龙 8 Gen 2 (SM8550)**, 12GB RAM |
| 开发机 | Windows 11 (Git Bash) |
| adb | `D:\code_env\android_sdk\platform-tools\adb.exe` (开启"USB 调试") |
| NDK | `D:\code_env\android_sdk\ndk\android-ndk-r26c` |
| cmake / ninja | `pip install cmake ninja` |
| llama.cpp | 本地源码 `../..llama.cpp` (最新版, 注意 `llama-cli` 已更名 `llama-idle`) |

## 目录结构

```
hello_world/
├── 0_environment_setup/           # ★ 共享环境: 模型/二进制/下载/编译/推送 (demo1&2 共用)
│   ├── models/                    # Qwen3-8B-Q4_K_M.gguf (4.7G), Qwen3-4B-Q4_K_M.gguf (2.4G)
│   ├── binaries/                  # llama-simple, llama-bench, llama-idle + lib*.so
│   └── scripts/{01,02,03,common.sh}
├── 1_inference_demo/              # ★ 本 demo: 只做推理 + TTFT/TPOT 测量
│   ├── scripts/
│   │   ├── 01~03                 # 薄封装, exec 到 0_environment_setup/scripts/
│   │   └── 04_run_ttft_tpot.sh   # 真机推理 + TTFT/TPOT 测量 + 写 CSV
│   └── results/                   # 运行日志 + TTFT_TPOT_results.csv
└── 2_inference_with_soc_settings/ # 下一个 demo: 推理 + 频率/功耗采集
```

> 模型、二进制、编译、推送都在 `0_environment_setup` 统一管理，本目录只保留测量逻辑。

## 四步走

```bash
# 1. 下载模型 (国内网络: 先 export HF_ENDPOINT=https://hf-mirror.com)
bash scripts/01_download_models.sh        # -> 0_environment_setup/models/

# 2. 交叉编译 (产物在 llama.cpp/build-android/bin/)
bash scripts/02_build_llama_android.sh    # -> 0_environment_setup/binaries/(手动拷贝) 

# 3. 推送二进制+模型到手机 (免 root, /data/local/tmp 为 shell 可写)
bash scripts/03_push_to_phone.sh          # 从 0_environment_setup 推送

# 4. 真机推理并测量 TTFT / TPOT
bash scripts/04_run_ttft_tpot.sh
```

> 注意：01~03 是共享脚本的薄封装，实际文件都落在 `../0_environment_setup`。
> 首次使用请先看 `../0_environment_setup/README.md`。

## TTFT / TPOT 测量口径

| 指标 | 定义 | 本 demo 的算法 |
|---|---|---|
| **TTFT** | 从请求发出到生成首个输出 token 的延迟 | ≈ `prompt eval time`(prefill) + 1 个 decode 步 |
| **TPOT** | 每个输出 token 的耗时(解码速度) | = `eval time / runs` (llama-simple) 或 `1000/tg_tps` (llama-bench) |

两种工具互为印证：

- **llama-simple**：真实生成（打印文本），结尾 `llama_perf_context_print` 给出
  `prompt eval time`（prefill）和 `eval time`（decode）——直接解析出 TTFT / TPOT。
- **llama-bench**：`-p 128 -n 64 -r 3 -o json`，给出 pp/tg tokens-per-second，
  折算 TTFT 与 TPOT。适合做多次平均的干净基准。

> 说明：本 demo 使用 CPU (XNNPACK/NEON, `-ngl 0`)，未用 NPU。
> 骁龙 8 Gen 2 的 NPU(QNN) 需另行接入，llama.cpp 主线暂不直接支持手机 NPU。

## 实测结果 (小米13 Pro, 2026-08-30, CPU/XNNPACK)

> 环境：Android 16, 骁龙 8 Gen 2, 12GB RAM（运行时系统已占用约 11GB，空闲仅 ~150MB，
> 后台进程已被 `am kill-all` 清理但 MIUI 常驻仍吃内存，数据受内存压力影响，属"尽力而为"非锁频基准）。

| 模型 | 方法 | Prompt 长度 | prefill | **TTFT** | **TPOT** | 生成速度 |
|---|---|---|---|---|---|---|
| Qwen3-4B Q4_K_M | llama-simple (真实生成) | 13 tok | 1144 ms | **1285 ms** | **141 ms** | 7.1 tok/s |
| Qwen3-4B Q4_K_M | llama-bench (pp128) | 128 tok | 12363 ms | **12500 ms** | 137 ms | 7.3 tok/s |
| Qwen3-8B Q4_K_M | llama-simple (真实生成) | 13 tok | 3335 ms | **3621 ms** | **286 ms** | 3.5 tok/s |
| Qwen3-8B Q4_K_M | llama-bench (pp128) | 128 tok | 23845 ms | **24097 ms** | 252 ms | 4.0 tok/s |

要点：
- **TTFT 主要被 prefill 支配**：prompt 越长 TTFT 越大（13 token vs 128 token 的 TTFT 差 ~10 倍）。
- **TPOT 只与模型大小相关**：4B≈140ms/token，8B≈260ms/token（骁龙 8 Gen 2 纯 CPU）。
- 两次运行之间 TPOT 波动 ~5%（137→141ms / 252→286ms），与后台内存抖动有关。

完整明细见 `results/TTFT_TPOT_results.csv`，原始日志在 `results/run_*/`。

## 注意事项

1. **内存**：Qwen3-8B Q4_K_M 约 4.7GB。12GB 机型建议跑前清理后台
   （脚本已自动 `am kill-all`）。若 8B 加载 OOM，把 `-c` 上下文调小或改用 4B。
2. **免 root**：全程 `/data/local/tmp` + adb shell，不需要 root。
   若要测量功耗/锁频，需 root（见 `2_inference_with_soc_settings` 方向）。
3. **无需模拟器/虚拟机**：模型在真机跑；PC 只负责交叉编译与 adb。
4. 生成中文请用 UTF-8 提示词；adb shell 传参建议用英文以避免转义问题。

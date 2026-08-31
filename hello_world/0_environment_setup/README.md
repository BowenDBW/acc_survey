# 0_environment_setup — 共享环境搭建（demo1 / demo2 共用）

> 目标：把各 demo 通用的"下载模型 / 交叉编译 llama.cpp / 推送到手机"集中到一处，
> demo1、demo2 只保留各自的"测量"逻辑，不再各自重复环境脚本。

## 目录结构

```
0_environment_setup/
├── README.md
├── scripts/
│   ├── common.sh                  # source 用: locate_adb / find_python / get_device / 手机端路径
│   ├── 01_download_models.sh      # 下载 Qwen3-8B/4B GGUF (Q4_K_M) 到 ./models
│   ├── 02_build_llama_android.sh  # NDK 交叉编译 llama.cpp -> arm64-v8a
│   └── 03_push_to_phone.sh        # 推送 ./binaries + ./models 到手机 /data/local/tmp/llama_demo
├── models/                        # 共享模型仓库 (~7.1GB, 只下一份)
│   ├── Qwen3-8B-Q4_K_M.gguf
│   └── Qwen3-4B-Q4_K_M.gguf
└── binaries/                      # 共享编译产物 (llama-simple / llama-bench / llama-idle + lib*.so)
```

## 前置环境（本机实测配置）

| 项 | 值 |
|---|---|
| 开发机 | Windows 11 (Git Bash) |
| 手机 | 小米 13 Pro (2210132C / nuwa), Android 16, 骁龙 8 Gen 2 (SM8550), 12GB |
| adb | `D:\code_env\android_sdk\platform-tools\adb.exe`（已开 USB 调试） |
| NDK | `D:\code_env\android_sdk\ndk\android-ndk-r26c` |
| cmake / ninja | `pip install cmake ninja`（ninja 在 Scripts 目录，可能需加入 PATH） |
| llama.cpp | 本地源码 `../../llama.cpp`，`LLAMA_DIR` 可覆盖 |
| python | miniconda 的 `python`（注意 Windows 的 `python3` 是 Store 占位符，`common.sh` 已自动规避） |

## 三步（各 demo 共用，只需执行一次）

```bash
# 1. 下载模型 (国内网络: 先 export HF_ENDPOINT=https://hf-mirror.com)
bash scripts/01_download_models.sh

# 2. NDK 交叉编译 -> llama.cpp/build-android/bin/ (llama-simple/llama-bench/llama-idle)
bash scripts/02_build_llama_android.sh

# 3. 推送到手机 /data/local/tmp/llama_demo/{bin,models} (免 root)
bash scripts/03_push_to_phone.sh
```

## 说明

1. **免 root**：全部落在 `/data/local/tmp`（shell 用户可写），不需要 root、不需要打包 APK。
2. **共享落点**：手机端 `/data/local/tmp/llama_demo/{bin,models}` 是 demo1 / demo2 的共同数据目录。
3. **编译口径**：`-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28`，
   `GGML_OPENMP=OFF`（Android 用 pthread），关闭 `LLAMA_BUILD_SERVER/APP` 跳过 Web UI 的 host 编译。
   本版本 llama.cpp 已将 `llama-cli` 更名为 `llama-idle`，真正可用的生成工具是 `llama-simple` / `llama-bench`。
4. **覆盖点**：`LLAMA_DIR`、`ANDROID_NDK`、`ADB_DEVICE` 均可通过环境变量覆盖，见各脚本头部。
5. demo1 / demo2 的 `scripts/01~03` 是薄封装，`exec` 到这里；想直接跑共享脚本，进入本目录执行即可。

## 常见问题

- **模型下载慢/失败**：`export HF_ENDPOINT=https://hf-mirror.com` 后重跑 01。
- **push 7.1GB 太慢**：只需 `models/` 时可用 `adb push` 断点续传（脚本已带重试/续传语义）。
- **编译报 `llama-ui-embed-host`**：说明 `LLAMA_BUILD_SERVER` 没关，检查脚本参数。

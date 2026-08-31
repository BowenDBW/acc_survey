# llama.cpp Vulkan 在 Adreno 740 上的调试记录

> 里程碑 M1：在小米 13 Pro（SM8550 / Adreno 740 / 无 root）上跑通 llama.cpp Vulkan GPU 推理，
> 并与 CPU 基线（llama.cpp CPU: ~8.58 tok/s）对比速度。
> 结论：**Vulkan 后端跑通，但 TG 仅 0.19 t/s，比 CPU 慢约 45 倍**，根因是驱动 bug + MM 路径对
> token 生成的结构性低效。已转 mllm（OpenCL 后端）重做此 demo，本文档为 Vulkan 阶段的完整记录。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 设备 | 小米 13 Pro（SM8550, Adreno 740, kalama 0x802a4001, Vulkan 1.3.128） |
| 系统 | Android（无 root） |
| 编译 | NDK r26c 交叉编译 arm64-v8a（MSVC + vcvars64 环境，vulkan-shaders-gen 用 cl 构建） |
| 源码 | `llama.cpp/`（无 git 历史，改动直接落在工作区） |
| 模型 | `Qwen3-4B-Q4_K_M.gguf`（/data/local/tmp/llama_demo/models） |
| 二进制 | `/data/local/tmp/llama_demo_gpu/bin` |
| 构建脚本 | `hello_world/3_gpu_inference_demo/scripts/build_llama_vulkan.sh`（→ build_vk.bat） |
| 推送 | `push_gpu.sh f6fed9b1` |
| 关键开关 | `GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1`（见 §3.1 驱动 bug） |

Adreno 740 设备能力（运行时打印）：
`warp size: 64 | shared memory: 32768 | int dot: 0 | matrix cores: none | fp16: 1`

## 2. 结果汇总

| 路径 | 状态 | 速度 |
|---|---|---|
| CPU 基线（llama.cpp CPU） | ✅ | TG ~8.58 tok/s |
| GPU mm 路径（原始 s_warptile，BM=32） | ✅ 能跑 | TG 0.16 t/s（6.36s/token） |
| GPU mm 路径（BM 32→16 warptile） | ✅ 能跑 | TG 0.19 t/s（5.23s/token）+18% |
| GPU vec 路径（n=1 专用内核） | ❌ 被驱动 bug 阻断 | 无法启用 |

## 3. 关键结论（按重要性）

### 3.1 驱动 bug：整数点积 shader 建管线必崩
含 `OpCapability Int8` 的 shader（`matmul_q4_k_q8_1_s`）`createComputePipeline` 返回 `ErrorUnknown`。
**规避**：`GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1`，强制走非整数 mm 路径（f32 累加）。这是必须的，否则启动即崩。

### 3.2 驱动 bug：vec 路径（MUL_MAT_VEC）被彻底阻断（决定性证据）
TG 专用内核 `mul_mat_vec_q4_k_f32_f32`（NUM_COLS=1 变体）在**完整 pipeline layout（12 个 descriptor binding + 52B push constant）**下
`createComputePipeline` 必然失败。这是用 vktest 实机数据（9 组 spec×layout 组合）确认的，非猜测：

- `NUM_COLS=1` + 完整 layout → **必失败**（spec{64,2,1}、{64,1,1}、{128,2,1}、{256,2,1} 全失败）
- `NUM_COLS>=2` + 完整 layout → 全通过（{64,2,2}、{64,2,3}、{64,2,8}、{256,2,2}…）
- 空 layout + NUM_COLS=1 → 通过

尝试过的修复（均无效）：
- `REQUIRE_FULL_SUBGROUPS + requiredSubgroupSize=64`（vktest 在空 layout 下验证有效，完整 layout 下无效）
- 修改 `ggml_vk_create_pipeline` 给 Qualcomm 的 mul_mat_vec 强制 req64+full → 实测仍 -13

**结论**：Adreno Vulkan 驱动对"1 列输出 + 多 binding"的 vec 管线有 bug，改 C++ 无法绕过（除非改 shader binding 布局，成本高且不确定），故 Qualcomm 上保留原厂绕过（跳过 vec，走 mm）。

### 3.3 mm 路径能跑，但 TG 结构性低效
- 管线惰性创建（首次 dispatch 才 createComputePipeline），启动时"全部成功"是假象。
- TG 期间 matmul 形态（插桩 `[VKDBG] mm`，仅 N<=4 时打印）：
  - `m=9728 n=1 k=2560`（q4_K）~28ms/次 —— 这是 `feed_forward_length=9728` 的投影，**M 大、N=1**
  - `m=2560 n=1 k=9728`（q6_K）~1.6ms —— K 维主导的归约
- **真正的大头是 FFN 大 matmul（N=10240/2560）**，被插桩过滤了（N>4），未单独计时；逐 op 统计确认 **MUL_MAT 绝对主导**（8064 次 / 171.6s，其余 op 合计 <5s）。
- MM 内核（`mul_mmq.comp`）每 workgroup 只 1 个 warp（BLOCK_SIZE=64 = WARP=64），
  对 M=1（TG 的 FFN）只有 1/32 行有效、对 N=1（注意力投影）只有 1/32 列有效 —— 结构性浪费。

### 3.4 warptile 实验（BM 32→16）带来 +18%
- `s_warptile_mmq`：`{64,32,32,32,32,32,2,2,2,1,64}` → `{64,16,32,32,16,32,2,2,2,1,64}`（BM/WM 32→16）
- 配套 `s_mmq_wg_denoms`：`{32,32,1}` → `{16,32,1}`（wg0 = ceil(M/BM)）
- 结果：TG 0.16 → 0.19 t/s（+18%）
- **BLOCK_SIZE 不能改**：A 加载的 stride 逻辑 `loadstride_a = BLOCK_SIZE*LOAD_VEC_A/BK` 锁死 64，
  改 128 会导致行覆盖不全（需改 shader，超出配置范围）。2 warps/wg 方案因此不可行。

## 4. 判断：为什么换 mllm

1. vec 路径（TG 正确解法）被驱动 bug 阻断，无法修复（§3.2）。
2. mm 路径对 TG 的结构性浪费（M=1/N=1）只能靠 BM/BN 微调缓解（+18%），天花板低。
3. mllm 无 Vulkan 后端，但 GPU 走 **OpenCL**（Adreno 740 支持 OpenCL 3.0，驱动通常比该 Vulkan bug 稳），
   且官方为手机/Adreno 优化，有现成 Android 构建路径与预转换 Qwen3-4B 模型。

## 5. 插桩与命令速查

```bash
# 构建（增量）
cd hello_world/3_gpu_inference_demo/scripts && ./build_llama_vulkan.sh

# 推送
./push_gpu.sh f6fed9b1

# 运行（必须禁用 int dot）
ADB=/d/code_env/android_sdk/platform-tools/adb.exe
$ADB -s f6fed9b1 shell "cd /data/local/tmp/llama_demo_gpu/bin && LD_LIBRARY_PATH=. \
  GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1 ./llama-simple \
  -m /data/local/tmp/llama_demo/models/Qwen3-4B-Q4_K_M.gguf -p 'Hello world' -n 6 -c 64"

# 逐 op 墙钟
GGML_VKDBG_PEROP=1 ./llama-simple ...
```

llama.cpp 中的改动位置（若需回滚参考）：
- `ggml-vulkan.cpp` `ggml_vk_mul_mat` dispatch：Qualcomm 跳过 vec 的绕过（vendor_id 检查）
- `ggml-vulkan.cpp` `s_warptile_mmq` / `s_mmq_wg_denoms`：BM=16 warptile
- 插桩：`[VKDBG] createComputePipeline`（~3078）、`[VKDBG] dispatch`（~8252）、`[VKDBG] mm`（~9370）、`GGML_VKDBG_PEROP`

## 6. 参考资料
- vktest 实机测试工具：`hello_world/3_gpu_inference_demo/scripts/vktest/`（验证 createComputePipeline 成败的独立小程序）
- 关键 shader：`llama.cpp/ggml/src/ggml-vulkan/vulkan-shaders/mul_mat_vec.comp`、`mul_mmq.comp`、`mul_mmq_funcs.glsl`

/* =====================================================================
 * neon_demo.c — ARM NEON 在 LLM 推理加速中的作用（演示 + 基准）
 *
 * 背景: 论文 mzCache [arXiv:2609.01338] 在 llama.cpp 上实现了端侧 LLM
 *   的多任务内存管理。其中 KV cache 的压缩/解压内核就是「使用 ARM NEON
 *   SIMD 指令实现」（论文 §5, 面向 ARMv8-A）——CPU 用它边恢复被逐出的
 *   KV 缓存、GPU 边做零等待推理, TTFT 降低 2.1-5.5×。
 *
 * 本 demo 在真机（小米 13 Pro / SM8550 / aarch64）上对比「可移植标量 C」
 * 与「手写 NEON intrinsics」:
 *
 *   ① KV cache 量化    f32 -> int8     （论文的压缩内核）
 *   ② KV cache 反量化  int8 -> f32     （论文的解压内核）
 *   ③ 量化 GEMM  Q8_0 × Q8_0           （推理主力算子, vdotq_s32 / SDOT）
 *
 * 编译（NDK r26c, aarch64）:
 *   aarch64-linux-android21-clang -O3 -march=armv8.2-a+dotprod -static \
 *       neon_demo.c -o neon_demo
 *
 * 说明: 标量基线用 #pragma clang loop vectorize(disable) 关掉了编译器
 *   自动向量化, 保证对比的是「手写 SIMD」 vs 「纯标量可移植写法」。
 * ===================================================================== */

#include <arm_neon.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <time.h>

/* ---------------- 形状参数（改这里即可换压力大小） ---------------- */
#define KV_ROWS 4096        /* KV cache 行数 ≈ token × 层          */
#define KV_N    1024        /* 每行元素 ≈ n_kv_heads × head_dim    */
                            /* 共 4M 个 f32 = 16 MiB                */

#define GEMM_M  64          /* M ≈ seq_len                         */
#define GEMM_K  4096        /* K ≈ hidden_dim                      */
#define GEMM_N  4096        /* N ≈ 输出维度                        */

#define QK8     32          /* Q8_0 block 大小（与 llama.cpp 一致） */

/* ---------------- HWCAP 探测（验证手机真的支持 SDOT） ------------ */
#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif
#ifndef HWCAP_ASIMD
#define HWCAP_ASIMD (1UL << 1)
#endif
/* asimddp 在 AT_HWCAP(第一个 word) 的 bit 20（不是 HWCAP2） */
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1UL << 20)
#endif
/* i8mm 在 AT_HWCAP2 的 bit 13（smmmla 矩阵乘） */
#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1UL << 13)
#endif

/* ---------------- 工具函数 ---------------- */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* 打印 CPU 对 NEON / int8 dot / i8mm 的硬件支持 */
static void print_cpu_caps(void) {
    unsigned long hw  = getauxval(AT_HWCAP);
    unsigned long hw2 = getauxval(AT_HWCAP2);
    printf("   CPU aarch64        : aarch64 (AArch64)\n");
    printf("   ASIMD (NEON, 128bit): %s\n", (hw  & HWCAP_ASIMD)    ? "YES" : "no");
    printf("   ASIMDDP (int8 dot) : %s\n",   (hw  & HWCAP_ASIMDDP) ? "YES" : "no");
    printf("   I8MM   (int8 matmul): %s\n",  (hw2 & HWCAP2_I8MM)   ? "YES" : "no");
    printf("  (SDOT = vdotq_s32, 一条指令 4 组 int8×int8→int32)\n");
}

/* 填充随机 f32, 范围 [-1,1], 模拟归一化后的激活 / KV cache */
static void fill_rand(float* x, long n, unsigned seed) {
    srand(seed);
    for (long i = 0; i < n; i++) x[i] = (float)(2.0 * rand() / RAND_MAX - 1.0);
}

/* =====================================================================
 * ① KV cache 量化 f32 -> int8（每行一个 scale, absmax 对称量化）
 *    对标 mzCache 论文 §5 的「压缩内核」
 * ===================================================================== */

static void quant_i8_scalar(const float* in, int8_t* out, float* scales,
                            int rows, int n) {
    for (int r = 0; r < rows; r++) {
        const float* p = in + (long)r * n;
        float amax = 0.f;
#pragma clang loop vectorize(disable)
        for (int i = 0; i < n; i++) { float a = fabsf(p[i]); if (a > amax) amax = a; }
        float scale = (amax == 0.f) ? 1.f : amax / 127.f;
        scales[r] = scale;
        float inv = 1.f / scale;
        int8_t* o = out + (long)r * n;
#pragma clang loop vectorize(disable)
        for (int i = 0; i < n; i++) {
            float q = roundf(p[i] * inv);
            if (q >  127.f) q =  127.f;
            if (q < -128.f) q = -128.f;
            o[i] = (int8_t)q;
        }
    }
}

static void quant_i8_neon(const float* in, int8_t* out, float* scales,
                          int rows, int n) {
    for (int r = 0; r < rows; r++) {
        const float* p = in + (long)r * n;
        /* absmax: 4 路同时求 max */
        float32x4_t vmax = vdupq_n_f32(0.f);
        int i = 0;
        for (; i + 4 <= n; i += 4)
            vmax = vmaxq_f32(vmax, vabsq_f32(vld1q_f32(p + i)));
        float amax = vmaxvq_f32(vmax);
        for (; i < n; i++) { float a = fabsf(p[i]); if (a > amax) amax = a; }

        float scale = (amax == 0.f) ? 1.f : amax / 127.f;
        scales[r] = scale;
        float inv = 1.f / scale;

        /* 量化: 一次处理 8 个 f32
         *   vcvt 取整 -> vqmovn 逐级饱和缩窄 int32->int16->int8 */
        float32x4_t vinv = vdupq_n_f32(inv);
        float32x4_t vhi  = vdupq_n_f32(127.f);
        float32x4_t vlo  = vdupq_n_f32(-128.f);
        int8_t* o = out + (long)r * n;
        i = 0;
        for (; i + 8 <= n; i += 8) {
            float32x4_t x0 = vmulq_f32(vld1q_f32(p + i),     vinv);
            float32x4_t x1 = vmulq_f32(vld1q_f32(p + i + 4), vinv);
            x0 = vmaxq_f32(vminq_f32(x0, vhi), vlo);
            x1 = vmaxq_f32(vminq_f32(x1, vhi), vlo);
            int32x4_t q0 = vcvtaq_s32_f32(x0);          /* ties-away, 同 roundf */
            int32x4_t q1 = vcvtaq_s32_f32(x1);
            int16x8_t s  = vcombine_s16(vqmovn_s32(q0), vqmovn_s32(q1));
            vst1_s8(o + i, vqmovn_s16(s));
        }
        for (; i < n; i++) {
            float q = roundf(p[i] * inv);
            if (q >  127.f) q =  127.f;
            if (q < -128.f) q = -128.f;
            o[i] = (int8_t)q;
        }
    }
}

/* =====================================================================
 * ② KV cache 反量化 int8 -> f32（论文 §5 的「解压内核」）
 * ===================================================================== */

static void dequant_i8_scalar(const int8_t* in, float* out, const float* scales,
                              int rows, int n) {
    for (int r = 0; r < rows; r++) {
        float s = scales[r];
#pragma clang loop vectorize(disable)
        for (int i = 0; i < n; i++) out[(long)r * n + i] = (float)in[(long)r * n + i] * s;
    }
}

static void dequant_i8_neon(const int8_t* in, float* out, const float* scales,
                            int rows, int n) {
    for (int r = 0; r < rows; r++) {
        const int8_t* p = in + (long)r * n;
        float* o        = out + (long)r * n;
        float32x4_t vsc = vdupq_n_f32(scales[r]);
        int i = 0;
        for (; i + 8 <= n; i += 8) {
            int8x8_t  b  = vld1_s8(p + i);
            int16x8_t s  = vmovl_s8(b);                       /* int8x8 -> int16x8 */
            int32x4_t lo = vmovl_s16(vget_low_s16(s));        /* int16 -> int32     */
            int32x4_t hi = vmovl_s16(vget_high_s16(s));
            vst1q_f32(o + i,     vmulq_f32(vcvtq_f32_s32(lo), vsc));
            vst1q_f32(o + i + 4, vmulq_f32(vcvtq_f32_s32(hi), vsc));
        }
        for (; i < n; i++) o[i] = (float)p[i] * scales[r];
    }
}

/* =====================================================================
 * ③ 量化 GEMM Q8_0 × Q8_0
 *    权重/激活先量化成 32 元素一 block、每 block 一个 scale 的 Q8_0
 *    （layout 与 llama.cpp 一致），核心用 vdotq_s32 一次算 4 个 int8 对。
 *    优势: int8 权重内存带宽只有 f32 的 1/4 + 一条指令 32 次乘加。
 * ===================================================================== */

typedef struct {
    float   d;              /* block scale: x ≈ q * d */
    int8_t  qs[QK8];
} block_q8_0;

static void quant_q8_blocks(const float* x, block_q8_0* y, int nblocks) {
    for (int b = 0; b < nblocks; b++) {
        const float* p = x + b * QK8;
        float amax = 0.f;
        for (int j = 0; j < QK8; j++) { float a = fabsf(p[j]); if (a > amax) amax = a; }
        float d = (amax == 0.f) ? 1.f : amax / 127.f;
        y[b].d = d;
        float inv = 1.f / d;
        for (int j = 0; j < QK8; j++) {
            float q = roundf(p[j] * inv);
            if (q >  127.f) q =  127.f;
            if (q < -127.f) q = -127.f;
            y[b].qs[j] = (int8_t)q;
        }
    }
}

static void gemm_f32_scalar(const float* A, const float* BT, float* C,
                            int M, int N, int K) {
    for (int m = 0; m < M; m++) {
        const float* a = A + (long)m * K;
        for (int n = 0; n < N; n++) {
            const float* b = BT + (long)n * K;
            float acc = 0.f;
#pragma clang loop vectorize(disable)
            for (int k = 0; k < K; k++) acc += a[k] * b[k];
            C[(long)m * N + n] = acc;
        }
    }
}

static void gemm_f32_neon(const float* A, const float* BT, float* C,
                          int M, int N, int K) {
    for (int m = 0; m < M; m++) {
        const float* a = A + (long)m * K;
        for (int n = 0; n < N; n++) {
            const float* b = BT + (long)n * K;
            float32x4_t acc = vdupq_n_f32(0.f);   /* 4 个独立累加器 */
            int k = 0;
            for (; k + 4 <= K; k += 4)
                acc = vmlaq_f32(acc, vld1q_f32(a + k), vld1q_f32(b + k));
            float c = vaddvq_f32(acc);
            for (; k < K; k++) c += a[k] * b[k];
            C[(long)m * N + n] = c;
        }
    }
}

static void gemm_q8_neon(const block_q8_0* qA, const block_q8_0* qBT, float* C,
                         int M, int N, int nb) {
    for (int m = 0; m < M; m++) {
        const block_q8_0* Arow = qA + (long)m * nb;
        for (int n = 0; n < N; n++) {
            const block_q8_0* Brow = qBT + (long)n * nb;
            float acc = 0.f;
            for (int i = 0; i < nb; i++) {
                int8x16_t a0 = vld1q_s8(Arow[i].qs);
                int8x16_t a1 = vld1q_s8(Arow[i].qs + 16);
                int8x16_t b0 = vld1q_s8(Brow[i].qs);
                int8x16_t b1 = vld1q_s8(Brow[i].qs + 16);
                int32x4_t s0 = vdotq_s32(vdupq_n_s32(0), a0, b0);
                int32x4_t s1 = vdotq_s32(vdupq_n_s32(0), a1, b1);
                int32_t   t  = vaddvq_s32(s0) + vaddvq_s32(s1);
                acc += (float)t * Arow[i].d * Brow[i].d;
            }
            C[(long)m * N + n] = acc;
        }
    }
}

static float rel_err(const float* a, const float* b, long n) {
    double diff = 0, norm = 0;
    for (long i = 0; i < n; i++) {
        double d = a[i] - b[i];
        diff += d * d;
        norm += (double)b[i] * b[i];
    }
    return (float)(sqrt(diff / norm));
}

static float max_abs_diff(const float* a, const float* b, long n) {
    float m = 0.f;
    for (long i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

/* =====================================================================
 * 基准入口
 * ===================================================================== */
int main(void) {
    const long KV_TOT = (long)KV_ROWS * KV_N;
    const long G_MN   = (long)GEMM_M * GEMM_N;
    const int  nb     = GEMM_K / QK8;

    printf("========== 7_arm_neon_demo — ARM NEON 在推理加速中的作用 ==========\n\n");
    printf("论文: mzCache [arXiv:2609.01338] 用 NEON SIMD 实现 KV cache 压缩/解压内核\n");
    printf("      (8-bit 量化 + CacheGen), CPU 边恢复 KV、GPU 边零等待推理,\n");
    printf("      TTFT 相比 storage-backed offload 降低 2.1-5.5x。\n\n");
    printf("本机 CPU 能力:\n");
    print_cpu_caps();
    printf("\n");

    /* ---- 缓冲区 ---- */
    float*  kv_in   = malloc(KV_TOT * sizeof(float));
    int8_t* q_sc    = malloc(KV_TOT);
    int8_t* q_neon  = malloc(KV_TOT);
    float*  dq_sc   = malloc(KV_TOT * sizeof(float));
    float*  dq_neon = malloc(KV_TOT * sizeof(float));
    float*  sc1     = malloc(KV_ROWS * sizeof(float));
    float*  sc2     = malloc(KV_ROWS * sizeof(float));

    float* A   = malloc((long)GEMM_M * GEMM_K * sizeof(float));
    float* BT  = malloc((long)GEMM_N * GEMM_K * sizeof(float));
    float* Cf  = malloc(G_MN * sizeof(float));   /* fp32 scalar 参考 */
    float* Cfn = malloc(G_MN * sizeof(float));   /* fp32 neon        */
    float* Cq8 = malloc(G_MN * sizeof(float));   /* int8 neon        */

    fill_rand(kv_in, KV_TOT, 1);
    fill_rand(A,   (long)GEMM_M * GEMM_K, 2);
    fill_rand(BT,  (long)GEMM_N * GEMM_K, 3);

    block_q8_0* qA   = malloc((long)GEMM_M * nb * sizeof(block_q8_0));
    block_q8_0* qBT  = malloc((long)GEMM_N * nb * sizeof(block_q8_0));
    quant_q8_blocks(A,  qA,  (long)GEMM_M * nb);
    quant_q8_blocks(BT, qBT, (long)GEMM_N * nb);

    /* ----------------------------------------------------------------
     * ①  KV cache 量化 f32->int8
     * ---------------------------------------------------------------- */
    printf("① KV cache 量化 f32 -> int8   (论文的压缩内核, 16MiB)\n");
    quant_i8_scalar(kv_in, q_sc, sc1, KV_ROWS, KV_N);
    quant_i8_neon  (kv_in, q_neon, sc2, KV_ROWS, KV_N);
    long ndiff = 0;
    for (long i = 0; i < KV_TOT; i++) if (q_sc[i] != q_neon[i]) ndiff++;
    int sdiff = 0;
    for (int r = 0; r < KV_ROWS; r++) if (sc1[r] != sc2[r]) sdiff++;

    for (int w = 0; w < 3; w++) quant_i8_scalar(kv_in, q_sc, sc1, KV_ROWS, KV_N);
    double best = 1e300;
    for (int i = 0; i < 15; i++) {
        double t0 = now_ms();
        quant_i8_scalar(kv_in, q_sc, sc1, KV_ROWS, KV_N);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double tq_sc = best;

    for (int w = 0; w < 3; w++) quant_i8_neon(kv_in, q_neon, sc2, KV_ROWS, KV_N);
    best = 1e300;
    for (int i = 0; i < 15; i++) {
        double t0 = now_ms();
        quant_i8_neon(kv_in, q_neon, sc2, KV_ROWS, KV_N);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double tq_ne = best;

    printf("   标量 : %8.3f ms\n", tq_sc);
    printf("   NEON : %8.3f ms   加速 %.1fx\n", tq_ne, tq_sc / tq_ne);
    printf("   正确性: 量化值差异 %ld/%ld 个, scale 差异 %d/%d 个 (0 即逐位一致)\n",
           ndiff, KV_TOT, sdiff, KV_ROWS);
    printf("\n");

    /* ----------------------------------------------------------------
     * ②  KV cache 反量化 int8->f32
     * ---------------------------------------------------------------- */
    printf("② KV cache 反量化 int8 -> f32 (论文的解压内核, 16MiB)\n");
    dequant_i8_scalar(q_sc, dq_sc, sc1, KV_ROWS, KV_N);
    dequant_i8_neon  (q_sc, dq_neon, sc1, KV_ROWS, KV_N);
    float qerr = max_abs_diff(dq_sc, dq_neon, KV_TOT);

    for (int w = 0; w < 3; w++) dequant_i8_scalar(q_sc, dq_sc, sc1, KV_ROWS, KV_N);
    best = 1e300;
    for (int i = 0; i < 15; i++) {
        double t0 = now_ms();
        dequant_i8_scalar(q_sc, dq_sc, sc1, KV_ROWS, KV_N);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double td_sc = best;

    for (int w = 0; w < 3; w++) dequant_i8_neon(q_sc, dq_neon, sc1, KV_ROWS, KV_N);
    best = 1e300;
    for (int i = 0; i < 15; i++) {
        double t0 = now_ms();
        dequant_i8_neon(q_sc, dq_neon, sc1, KV_ROWS, KV_N);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double td_ne = best;

    printf("   标量 : %8.3f ms\n", td_sc);
    printf("   NEON : %8.3f ms   加速 %.1fx\n", td_ne, td_sc / td_ne);
    printf("   正确性: 与标量最大绝对差 = %.3e (反量化后 f32)\n", qerr);
    printf("\n");

    /* ----------------------------------------------------------------
     * ③  量化 GEMM  Q8_0 × Q8_0  (M×K×N)
     * ---------------------------------------------------------------- */
    printf("③ 量化 GEMM  fp32 vs int8 NEON   (%d×%d×%d)\n", GEMM_M, GEMM_K, GEMM_N);

    /* 参考: fp32 标量 */
    gemm_f32_scalar(A, BT, Cf, GEMM_M, GEMM_N, GEMM_K);
    gemm_f32_neon  (A, BT, Cfn, GEMM_M, GEMM_N, GEMM_K);
    gemm_q8_neon   (qA, qBT, Cq8, GEMM_M, GEMM_N, nb);

    float e_f32 = max_abs_diff(Cf, Cfn, G_MN);
    float e_q8  = rel_err(Cq8, Cf, G_MN);

    for (int w = 0; w < 1; w++) gemm_f32_scalar(A, BT, Cf, GEMM_M, GEMM_N, GEMM_K);
    best = 1e300;
    for (int i = 0; i < 3; i++) {
        double t0 = now_ms();
        gemm_f32_scalar(A, BT, Cf, GEMM_M, GEMM_N, GEMM_K);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double tg_sc = best;

    for (int w = 0; w < 1; w++) gemm_f32_neon(A, BT, Cfn, GEMM_M, GEMM_N, GEMM_K);
    best = 1e300;
    for (int i = 0; i < 5; i++) {
        double t0 = now_ms();
        gemm_f32_neon(A, BT, Cfn, GEMM_M, GEMM_N, GEMM_K);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double tg_ne = best;

    for (int w = 0; w < 1; w++) gemm_q8_neon(qA, qBT, Cq8, GEMM_M, GEMM_N, nb);
    best = 1e300;
    for (int i = 0; i < 5; i++) {
        double t0 = now_ms();
        gemm_q8_neon(qA, qBT, Cq8, GEMM_M, GEMM_N, nb);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    double tg_q8 = best;

    const double FLOP = 2.0 * GEMM_M * GEMM_K * GEMM_N;   /* 乘加 = 2 flop */
    printf("   fp32 标量  : %8.3f ms    %6.1f GFLOPS\n",
           tg_sc, FLOP / (tg_sc * 1e-3) / 1e9);
    printf("   fp32 NEON  : %8.3f ms    %6.1f GFLOPS   (vmlaq_f32, 4 路 FMA)\n",
           tg_ne, FLOP / (tg_ne * 1e-3) / 1e9);
    printf("   int8 NEON  : %8.3f ms    %6.1f GFLOPS   (vdotq_s32 SDOT, 权重 1/4 内存)\n",
           tg_q8, FLOP / (tg_q8 * 1e-3) / 1e9);
    printf("   NEON fp32 vs 标量 最大绝对差 : %.3e\n", e_f32);
    printf("   int8 相对误差(参考 fp32 标量): %.4f  (≈ Q8_0 精度, 可接受)\n", e_q8);
    printf("   int8 相对 fp32 NEON 加速     : %.1fx\n", tg_ne / tg_q8);
    printf("   int8 相对 fp32 标量加速      : %.1fx\n", tg_sc / tg_q8);
    printf("\n");

    printf("结论: NEON 在推理里同时给「计算吞吐」和「内存带宽」两处加速——\n");
    printf("  量化 GEMM 用 SDOT 把每次 4 个 int8 对乘加压成一条指令,\n");
    printf("  权重内存也从 4 字节/数降到 1 字节/数;\n");
    printf("  而 KV cache 量化/反量化(本 demo ①②, 论文 mzCache 的例子)\n");
    printf("  让被逐出的 KV 能极快地压缩进内存、再由 CPU 并行恢复。\n");

    return 0;
}

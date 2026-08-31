#!/bin/bash
# ============================================================
# 04_run_ttft_tpot.sh
# 在手机(小米13 Pro, 已 adb 连接)上用 llama.cpp 跑 Qwen3-4B / Qwen3-8B,
# 测量 TTFT (Time To First Token) 和 TPOT (Time Per Output Token)。
#
# 两种测量口径:
#   1) llama-simple 真实生成: 从结尾 perf 输出解析 prompt eval / eval 耗时
#        TTFT ≈ prompt_eval_ms + 1×decode   (首个 token 在 prefill 结束后出现)
#        TPOT =  eval_ms / runs             (每个输出 token 的解码耗时)
#   2) llama-bench 基准:      pp/tg tokens-per-second 折算
#        TTFT(prompt L) ≈ L/pp_tps + 1/tg_tps,  TPOT = 1/tg_tps
#
# 结果写入 results/TTFT_TPOT_results.csv
# ============================================================
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p results
# 共享环境: MSYS 路径转换防护 + locate_adb/find_python/get_device + 手机端目录
source "$(dirname "$0")/../../0_environment_setup/scripts/common.sh"

locate_adb || { echo "[错误] 未找到 adb, 请安装 platform-tools 或设置 PATH"; exit 1; }
PY="$(find_python)" || { echo "[错误] 未找到可用的 python"; exit 1; }
echo "[python] $PY ($($PY --version 2>&1))"

DEV="$(get_device)"
[ -z "$DEV" ] && { echo "[错误] 未检测到设备"; exit 1; }
ADB="adb -s $DEV"

BIN=$DEV_BIN_DIR
MODELS=$DEV_MODELS_DIR

PROMPT="What is the capital of France? Answer in one short sentence."
N_GEN=64
THREADS=8
TS=$(date +%Y%m%d_%H%M%S)
LOGDIR="results/run_$TS"
mkdir -p "$LOGDIR"
CSV="results/TTFT_TPOT_results.csv"
[ -f "$CSV" ] || echo "device,model,method,n_prompt_tokens,n_gen_tokens,prompt_eval_ms,TTFT_ms,TPOT_ms,tg_tokens_per_s,notes" > "$CSV"

echo "[设备] $DEV  | 时间戳 $TS"
echo "[内存] 运行前:"
$ADB shell free -h | head -2

# 释放后台进程内存(免 root 的尽力而为)
$ADB shell am kill-all 2>/dev/null || true
sleep 2
echo "[内存] 清理后台后:"
$ADB shell free -h | head -2

# ---------- llama-simple: 真实生成 + perf 解析 ----------
run_simple() {
  local MODEL="$1" NAME="$2"
  echo ""
  echo "========== llama-simple / $NAME (真实生成, n=$N_GEN) =========="
  local LOG="$LOGDIR/simple_$NAME.log"
  $ADB shell "cd $BIN && LD_LIBRARY_PATH=. ./llama-simple -m $MODELS/$MODEL -n $N_GEN -ngl 0 \"$PROMPT\"" 2>&1 | tee "$LOG" > /dev/null
  # 由 python 统一解析, 避免 bash 正则踩坑
  "$PY" - "$LOG" "$CSV" "$DEV" "$NAME" <<'PY'
import re, sys, csv
log, csvp, dev, name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
raw = open(log, encoding='utf-8', errors='ignore').read().replace('\r','')
def val(pat):
    m = re.search(pat, raw)
    return m.group(1) if m else None
load_ms  = val(r'llama_perf_context_print:\s+load time =\s+([0-9.]+) ms')
pe_ms    = val(r'prompt eval time =\s+([0-9.]+) ms')
pt       = val(r'prompt eval time =\s+[0-9.]+ ms\s*/\s*([0-9]+) tokens')
et_ms    = val(r'llama_perf_context_print:\s+eval time =\s+([0-9.]+) ms')
er       = val(r'eval time =\s+[0-9.]+ ms\s*/\s*([0-9]+) runs')
if pe_ms and et_ms and er and float(er) > 0:
    tpot = float(et_ms)/float(er)
    ttft = float(pe_ms) + tpot          # prefill + 首个 decode 步
    tps  = 1000.0/tpot
    print(f">> {name} [llama-simple] load={load_ms}ms prefill={pe_ms}ms/{pt}tok -> TTFT~{ttft:.1f}ms, TPOT={tpot:.2f}ms ({tps:.2f} tok/s)")
    with open(csvp,'a',newline='') as f:
        csv.writer(f).writerow([dev,name,'llama-simple',pt,er,f"{float(pe_ms):.2f}",f"{ttft:.2f}",f"{tpot:.2f}",f"{tps:.2f}",'real generation'])
else:
    print(f"[!] {name} 解析失败: pe={pe_ms} et={et_ms} runs={er}; 原始 perf 行:")
    for l in raw.splitlines():
        if 'llama_perf_context_print' in l: print("   ", l.strip())
PY
}

# ---------- llama-bench: 干净基准 (JSON) ----------
run_bench() {
  local MODEL="$1" NAME="$2" P_LEN=128 G_LEN=64
  echo ""
  echo "========== llama-bench / $NAME (pp=$P_LEN tg=$G_LEN, x3) =========="
  local LOG="$LOGDIR/bench_$NAME.log"
  $ADB shell "cd $BIN && LD_LIBRARY_PATH=. ./llama-bench -m $MODELS/$MODEL -p $P_LEN -n $G_LEN -r 3 -t $THREADS -o json" 2>&1 | tee "$LOG" > /dev/null
  "$PY" - "$LOG" "$CSV" "$DEV" "$NAME" <<'PY'
import json, sys, csv
log, csvp, dev, name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
raw = open(log, encoding='utf-8', errors='ignore').read().replace('\r','')
# 兼容 {"tests":[...]} 与裸数组 [...] 两种输出
data = None
for parse in (lambda: json.loads(raw),):
    try:
        data = parse(); break
    except Exception:
        s, e = raw.find('['), raw.rfind(']')
        if s >= 0 and e > s:
            try: data = json.loads(raw[s:e+1]); break
            except Exception: pass
if data is None:
    print(f"[!] {name} bench JSON 解析失败"); sys.exit(0)
if isinstance(data, dict):
    data = data.get('tests', [])
# llama-bench 两个 test 的速度都放在 avg_ts:
#   pp test: n_gen=0, avg_ts = prompt处理 tokens/s
#   tg test: n_prompt=0, avg_ts = 文本生成 tokens/s
pp_tps = tg_tps = None
n_p = n_g = 0
for t in data:
    spd = t.get('avg_ts') or 0
    if t.get('n_prompt'): pp_tps, n_p = spd, t['n_prompt']
    if t.get('n_gen'):    tg_tps, n_g = spd, t['n_gen']
if pp_tps and tg_tps:
    prefill_ms = n_p/pp_tps*1000
    ttft = prefill_ms + 1000/tg_tps
    tpot = 1000/tg_tps
    print(f">> {name} [llama-bench] pp={pp_tps:.2f} tok/s, tg={tg_tps:.2f} tok/s -> TTFT~{ttft:.1f}ms TPOT={tpot:.2f}ms")
    with open(csvp,'a',newline='') as f:
        csv.writer(f).writerow([dev,name,'llama-bench',n_p,n_g,f"{prefill_ms:.2f}",f"{ttft:.2f}",f"{tpot:.2f}",f"{tg_tps:.2f}",'benchmark'])
else:
    print(f"[!] {name} bench 未同时拿到 pp/tg: pp={pp_tps} tg={tg_tps}")
PY
}

# ---- 依次跑 4B 和 8B ----
run_simple "Qwen3-4B-Q4_K_M.gguf"  "Qwen3-4B"
run_bench  "Qwen3-4B-Q4_K_M.gguf"  "Qwen3-4B"
run_simple "Qwen3-8B-Q4_K_M.gguf"  "Qwen3-8B"
run_bench  "Qwen3-8B-Q4_K_M.gguf"  "Qwen3-8B"

echo ""
echo "========== 汇总 =========="
cat "$CSV"
echo ""
echo "完整日志: $LOGDIR/"

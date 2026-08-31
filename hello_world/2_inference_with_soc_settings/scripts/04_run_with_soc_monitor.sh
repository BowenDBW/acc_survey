#!/bin/bash
# ============================================================
# 04_run_with_soc_monitor.sh — demo2: 推理 + 芯片频率/功耗/温度采集
#
# 在推理(TTFT/TPOT 测量)的同时, 用手机端 soc_sampler.sh 并行采样:
#   - CPU 三簇频率 (免 root 可读)
#   - 温度 (cpuss / battery, 免 root 可读)
#   - CPU 负载 (/proc/stat, 免 root 可读)
#   - 若 root: 电池瞬时功率 power_now(uW) -> 平均/峰值功耗与能量(J) + GPU 频率/占用
# 所有重要参数都在 ../config.json 里调, 本脚本不硬编码;
# 命令行参数可覆盖部分选项 (见 --help), 不指定的选项走 config.json 默认值。
#
# 用法: bash scripts/04_run_with_soc_monitor.sh [--label NAME] [--pin-cpu7 max] ...
# ============================================================
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p results
source "$(dirname "$0")/../../0_environment_setup/scripts/common.sh"

locate_adb || { echo "[错误] 未找到 adb"; exit 1; }
PY="$(find_python)" || { echo "[错误] 未找到可用的 python"; exit 1; }
DEV="$(get_device)"
[ -z "$DEV" ] && { echo "[错误] 未检测到设备"; exit 1; }
ADB="adb -s $DEV"
echo "[设备] $DEV | python=$PY"

# ---- 解析 config.json -> 环境变量 ----
# 注意: 用当前目录下的相对临时文件, 不用 mktemp(/tmp)。
# 因为 MSYS_NO_PATHCONV 下 Windows python 会把 /tmp/... 解析成 D:\tmp\...,
# 与 bash 的 MSYS /tmp 不一致。
CFG_ENV=".cfg_env.$$"
"$PY" - "config.json" "$CFG_ENV" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], encoding='utf-8'))
out = open(sys.argv[2], 'w', encoding='utf-8')
def emit(k, v):
    if isinstance(v, bool):   line = f"{k}={'1' if v else '0'}"
    elif isinstance(v, (int, float)): line = f"{k}={v}"
    else:                     line = f"{k}={v!r}"
    out.write(line + "\n")
run  = cfg['run']; soc = cfg['soc']; samp = cfg['sampling']; pw = cfg['power']
for k, v in run.items():  emit('CFG_' + k.upper(), v)
pin = soc.get('pin_cpu_freqs', soc.get('pin_cpu_freqs_khz', {}))   # 兼容旧 key 名
emit('CFG_CPU02', pin.get('cpu0-2', 0))
emit('CFG_CPU36', pin.get('cpu3-6', 0))
emit('CFG_CPU7',  pin.get('cpu7', 0))
emit('CFG_GPU',   soc.get('pin_gpu_freq', soc.get('pin_gpu_freq_khz', 0)))
emit('CFG_GOV_AFTER', soc.get('governor_after', 'schedutil'))
emit('CFG_INTERVAL_MS',  samp.get('interval_ms', 100))
emit('CFG_IDLE_WARMUP_S', samp.get('idle_warmup_s', 5))
emit('CFG_IDLE_MEASURE_S', samp.get('idle_measure_s', 5))
emit('CFG_POWER_MODE', pw.get('mode', 'auto'))
out.close()
PY
source "$CFG_ENV"; rm -f "$CFG_ENV"

# ---- 命令行参数覆盖 (不指定的选项走 config.json 默认值) ----
show_help() {
  cat <<'EOF'
用法: bash scripts/04_run_with_soc_monitor.sh [选项...]
不指定任何选项时, 全部参数用 config.json 的默认值。

可覆盖的选项 (均为 <选项> <值> 两段式):
  --label NAME            run 目录名前缀 (默认 config.run.label)
  --mode simple|bench     推理工具 (默认 config.run.mode)
  --model FILE            模型文件名 (默认 config.run.model)
  --prompt "TEXT"         生成提示词 (默认 config.run.prompt)
  --n-gen N               生成 token 数, simple 用 (默认 config.run.n_gen)
  --n-ctx N               上下文长度 (默认 config.run.n_ctx)
  --threads N             CPU 线程数 (默认 config.run.threads)
  --pin-cpu02 V           锁小核簇 cpu0-2 (0/max/min/档位1..16/MHz)
  --pin-cpu36 V           锁中核簇 cpu3-6 (0/max/min/档位1..19/MHz)
  --pin-cpu7 V            锁大核簇 cpu7   (0/max/min/档位1..20/MHz)
  --pin-gpu V             锁 GPU (0/max/min/档位/MHz, 需 root 读表)
  --interval-ms N         采样周期 ms (默认 config.sampling.interval_ms)
  --idle-warmup N         空闲基线预热 s (默认 config.sampling.idle_warmup_s)
  --idle-measure N        空闲基线测量 s (默认 config.sampling.idle_measure_s)
  --power auto|always     功耗模式 (默认 config.power.mode)
  --help, -h              显示本帮助并退出

示例:
  bash scripts/04_run_with_soc_monitor.sh --pin-cpu7 max --label big-max
  bash scripts/04_run_with_soc_monitor.sh --n-gen 128 --threads 4 --label short-gen
EOF
}
while [ $# -gt 0 ]; do
  case "$1" in
    --label)        CFG_LABEL="$2";        shift 2 ;;
    --mode)         CFG_MODE="$2";         shift 2 ;;
    --model)        CFG_MODEL="$2";        shift 2 ;;
    --prompt)       CFG_PROMPT="$2";       shift 2 ;;
    --n-gen)        CFG_N_GEN="$2";        shift 2 ;;
    --n-ctx)        CFG_N_CTX="$2";        shift 2 ;;
    --threads)      CFG_THREADS="$2";      shift 2 ;;
    --pin-cpu02)    CFG_CPU02="$2";        shift 2 ;;
    --pin-cpu36)    CFG_CPU36="$2";        shift 2 ;;
    --pin-cpu7)     CFG_CPU7="$2";         shift 2 ;;
    --pin-gpu)      CFG_GPU="$2";          shift 2 ;;
    --interval-ms)  CFG_INTERVAL_MS="$2";  shift 2 ;;
    --idle-warmup)  CFG_IDLE_WARMUP_S="$2"; shift 2 ;;
    --idle-measure) CFG_IDLE_MEASURE_S="$2"; shift 2 ;;
    --power)        CFG_POWER_MODE="$2";   shift 2 ;;
    --help|-h)      show_help; exit 0 ;;
    *) echo "[错误] 未知参数: $1 (用 --help 查看支持项)"; exit 1 ;;
  esac
done

echo "[config] model=$CFG_MODEL mode=$CFG_MODE n_gen=$CFG_N_GEN threads=$CFG_THREADS"
echo "[config] 锁频输入 CPU02=$CFG_CPU02 CPU36=$CFG_CPU36 CPU7=$CFG_CPU7 GPU=$CFG_GPU (0=不锁 / max|min / 档位 1..N / MHz)"
echo "[config] 锁频 governor 还原=$CFG_GOV_AFTER"

BIN=$DEV_BIN_DIR
MODELS=$DEV_MODELS_DIR
DEV_TMP=/data/local/tmp/llama_demo
SLEEP_SEC=$(awk "BEGIN{printf \"%.3f\", $CFG_INTERVAL_MS/1000}")

TS=$(date +%Y%m%d_%H%M%S)
RUN_DIR="results/${CFG_LABEL:-run}_$TS"
CSV_ALL="results/soc_results.csv"
mkdir -p "$RUN_DIR"
echo "[run] 输出目录: $RUN_DIR/"

# ---- 探测 root 能力 ----
ROOT=0; POWER_OK=0; GPU_OK=0
if $ADB shell "command -v su >/dev/null 2>&1"; then
  if $ADB shell "timeout 5 su -c 'id'" 2>/dev/null | grep -q 'uid=0'; then
    ROOT=1
    $ADB shell "timeout 5 su -c 'cat /sys/class/power_supply/battery/power_now'" 2>/dev/null | grep -qE '^[0-9]+$' && POWER_OK=1
    $ADB shell "timeout 5 su -c 'cat /sys/class/kgsl/kgsl-3d0/gpuclk'" 2>/dev/null | grep -qE '^[0-9]+$' && GPU_OK=1
  fi
fi
if [ "$CFG_POWER_MODE" = "always" ] && [ "$POWER_OK" != "1" ]; then
  echo "[错误] power.mode=always 但本机无 root / 读不到功耗, 请改为 auto 或给手机 root"; exit 1
fi
echo "[root] ROOT=$ROOT POWER_OK=$POWER_OK GPU_OK=$GPU_OK"

# ---- (root) 锁频: 保存原值 -> 设置目标值 ----
declare -a MIN_BEFORE=() MAX_BEFORE=()
pin_cpu() { # $1=cpu核心号 $2=目标频率(kHz), 0=跳过
  local c="$1" f="$2"
  [ "$f" = "0" ] && return
  MIN_BEFORE[$c]=$($ADB shell "timeout 5 su -c 'cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_min_freq'" 2>/dev/null | tr -d '\r')
  MAX_BEFORE[$c]=$($ADB shell "timeout 5 su -c 'cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq'" 2>/dev/null | tr -d '\r')
  $ADB shell "timeout 5 su -c 'echo $f > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq'" >/dev/null 2>&1
  $ADB shell "timeout 5 su -c 'echo $f > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_min_freq'" >/dev/null 2>&1
  echo "[pin] cpu$c -> $f kHz"
}
restore_cpu() {
  local c
  for c in 0 1 2 3 4 5 6 7; do
    if [ -n "${MAX_BEFORE[$c]:-}" ]; then
      $ADB shell "timeout 5 su -c 'echo ${MAX_BEFORE[$c]} > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq'" >/dev/null 2>&1
      $ADB shell "timeout 5 su -c 'echo ${MIN_BEFORE[$c]} > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_min_freq'" >/dev/null 2>&1
    fi
  done
  [ -n "${CFG_GOV_AFTER:-}" ] && $ADB shell "timeout 5 su -c 'echo $CFG_GOV_AFTER > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'" >/dev/null 2>&1
}

# ---- 锁频输入换算: 把用户友好的值翻译成 sysfs 实际数值 ----
# 输入 $1=簇标签(cpu02|cpu36|cpu7|gpu)  $2=config 里的值 -> 输出 sysfs 数值 (0=跳过)
# 支持: 0=不锁 | max/min=最高/最低档 | 1..100=档位序号(1=最低档) |
#       纯整数>100 或 <小数>M = MHz(如 2016 或 2016.5M) | <小数>G = GHz(如 2.0G)
# CPU 档位表单位 kHz, GPU 档位表单位 Hz, 都按各自表原生单位返回(吸附到最近档)。
resolve_freq() {
  local clu="$1" inp="$2" path div avail t
  case "$clu" in
    cpu02) path=/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies; div=1000 ;;
    cpu36) path=/sys/devices/system/cpu/cpu3/cpufreq/scaling_available_frequencies; div=1000 ;;
    cpu7)  path=/sys/devices/system/cpu/cpu7/cpufreq/scaling_available_frequencies; div=1000 ;;
    gpu)   path=/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies; div=1000000 ;;
  esac
  if [ "$clu" = "gpu" ]; then
    avail=$($ADB shell "timeout 5 su -c 'cat $path'" 2>/dev/null | tr -d '\r')   # GPU 表需 root
  else
    avail=$($ADB shell "cat $path" 2>/dev/null | tr -d '\r')
  fi
  if [ -z "$avail" ]; then
    echo "[warn] 读不到 $clu 可用频率表($path), 跳过该簇锁频" >&2; echo 0; return
  fi
  case "$inp" in
    0)      echo 0; return ;;
    max|MAX|Max) echo "$avail" | awk '{print $NF}'; return ;;
    min|MIN|Min) echo "$avail" | awk '{print $1}';  return ;;
  esac
  # 档位序号: 纯整数 <=100 (1=最低档=表第1个)
  if echo "$inp" | grep -qE '^[0-9]+$'; then
    if [ "$inp" -le 100 ]; then
      echo "$avail" | awk -v i="$inp" '{ if(i>=1 && i<=NF) print $i; else { print "档位越界: 1.."NF > "/dev/stderr"; print 0 } }'
    else
      # 纯整数 >100 视为 MHz
      echo "$avail" | awk -v t="$inp" -v d="$div" '{ best=-1; r=$1; for(i=1;i<=NF;i++){ v=$i/d; dist=(v>t)?(v-t):(t-v); if(best<0||dist<best){best=dist; r=$i} } print r }'
    fi
    return
  fi
  # 带 M / G 后缀, 或裸小数(视为 GHz)
  if echo "$inp" | grep -qiE '^[0-9]+(\.[0-9]+)?[Mm]$'; then
    t=$(echo "$inp" | tr -d 'Mm')
  elif echo "$inp" | grep -qiE '^[0-9]+(\.[0-9]+)?[Gg]$'; then
    t=$(echo "$inp" | tr -d 'Gg' | awk '{print $1*1000}')
  elif echo "$inp" | grep -qE '^[0-9]+\.[0-9]+$'; then
    t=$(echo "$inp" | awk '{print $1*1000}')   # 裸小数 -> GHz
  else
    echo "[warn] 无法识别锁频输入 '$inp' (支持 0/max/min/档位1..N/MHz如2016/GHz如2.0G)" >&2; echo 0; return
  fi
  echo "$avail" | awk -v t="$t" -v d="$div" '{ best=-1; r=$1; for(i=1;i<=NF;i++){ v=$i/d; dist=(v>t)?(v-t):(t-v); if(best<0||dist<best){best=dist; r=$i} } print r }'
}

GPU_PINNED=0
if [ "$ROOT" = "1" ]; then
  # 先换算成 sysfs 实际值, 再锁频
  CPU02_KHZ=$(resolve_freq cpu02 "$CFG_CPU02")
  CPU36_KHZ=$(resolve_freq cpu36 "$CFG_CPU36")
  CPU7_KHZ=$(resolve_freq cpu7 "$CFG_CPU7")
  GPU_HZ=$(resolve_freq gpu "$CFG_GPU")
  echo "[pin] 换算: CPU02=$CFG_CPU02->$CPU02_KHZ, CPU36=$CFG_CPU36->$CPU36_KHZ, CPU7=$CFG_CPU7->$CPU7_KHZ, GPU=$CFG_GPU->$GPU_HZ"
  # CPU 三簇各核心
  for c in 0 1 2; do pin_cpu "$c" "$CPU02_KHZ"; done
  for c in 3 4 5 6; do pin_cpu "$c" "$CPU36_KHZ"; done
  pin_cpu 7 "$CPU7_KHZ"
  if [ "$GPU_HZ" != "0" ]; then
    $ADB shell "timeout 5 su -c 'echo $GPU_HZ > /sys/class/kgsl/kgsl-3d0/max_gpuclk'" >/dev/null 2>&1 && GPU_PINNED=1
    echo "[pin] gpu -> $GPU_HZ Hz (GPU_PINNED=$GPU_PINNED)"
  fi
  sleep 1
else
  echo "[info] 非 root: 跳过锁频 (本机 CPU/GPU 频率锁频需要 root)"
fi

# ---- 推送采样器 ----
echo "[1/5] 推送 soc_sampler.sh 到手机 ..."
$ADB push scripts/soc_sampler.sh "$DEV_TMP/soc_sampler.sh" >/dev/null
$ADB shell chmod +x "$DEV_TMP/soc_sampler.sh"

# ---- 采样器启动/停止辅助 ----
# start_sampler <outfile>: 后台启动 (root 则用 su 起, 否则普通 shell)
start_sampler() {
  local OUT="$1"
  $ADB shell "rm -f $DEV_TMP/stop_soc $OUT"
  if [ "$ROOT" = "1" ]; then
    $ADB shell "nohup su -c 'sh $DEV_TMP/soc_sampler.sh $OUT $DEV_TMP/stop_soc 1 $SLEEP_SEC' >/dev/null 2>&1 &"
  else
    $ADB shell "nohup sh $DEV_TMP/soc_sampler.sh $OUT $DEV_TMP/stop_soc 0 $SLEEP_SEC >/dev/null 2>&1 &"
  fi
  sleep 0.5
}
stop_sampler() {
  $ADB shell "touch $DEV_TMP/stop_soc" 2>/dev/null
  sleep 0.6   # 让采样器写完最后一笔
}

# ---- 跑前空闲基线 (idle 段的频率/功耗作为对比基准) ----
echo "[2/5] 空闲基线采样 (warmup ${CFG_IDLE_WARMUP_S}s + measure ${CFG_IDLE_MEASURE_S}s) ..."
sleep "$CFG_IDLE_WARMUP_S"
start_sampler "$DEV_TMP/soc_idle.out"
sleep "$CFG_IDLE_MEASURE_S"
stop_sampler
$ADB pull "$DEV_TMP/soc_idle.out" "$RUN_DIR/soc_idle.csv" >/dev/null 2>&1 || true

# ---- 推理 + 并行采样 ----
# 先清理后台进程释放内存 (免 root 的尽力而为, 与 demo1 口径一致)
echo "[3/5] 释放后台内存 + 开始推理与采样 ($CFG_MODE, $CFG_MODEL) ..."
$ADB shell am kill-all 2>/dev/null || true
sleep 2
INF_OUT="$DEV_TMP/soc_inf.out"
start_sampler "$INF_OUT"
INF_LOG="$RUN_DIR/inference.log"
$ADB shell "echo MARK_INFERENCE_START \$(date +%s%N) >> $INF_OUT"

if [ "$CFG_MODE" = "bench" ]; then
  $ADB shell "cd $BIN && LD_LIBRARY_PATH=. ./llama-bench -m $MODELS/$CFG_MODEL -p 128 -n 64 -r 3 -t $CFG_THREADS -o json" 2>&1 | tee "$INF_LOG" >/dev/null
else
  $ADB shell "cd $BIN && LD_LIBRARY_PATH=. ./llama-simple -m $MODELS/$CFG_MODEL -n $CFG_N_GEN -c $CFG_N_CTX -ngl 0 -t $CFG_THREADS \"$CFG_PROMPT\"" 2>&1 | tee "$INF_LOG" >/dev/null
fi
$ADB shell "echo MARK_INFERENCE_END \$(date +%s%N) >> $INF_OUT"
stop_sampler
$ADB pull "$INF_OUT" "$RUN_DIR/soc_samples.csv" >/dev/null 2>&1 || true

# ---- 恢复锁频 ----
[ "$ROOT" = "1" ] && restore_cpu
echo "[4/5] 采样文件已拉取: soc_samples.csv / soc_idle.csv"

# ---- 汇总分析 (python): 频率/温度/负载/功耗 + TTFT/TPOT ----
echo "[5/5] 汇总分析 ..."
"$PY" - "$RUN_DIR" "$CSV_ALL" "$DEV" "$ROOT" "$POWER_OK" "$GPU_OK" "$CFG_INTERVAL_MS" "$CFG_MODEL" <<'PY'
import sys, re, os, json, csv
rundir, csv_all, dev, root, power_ok, gpu_ok, interval, model = \
    sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6], sys.argv[7], sys.argv[8]
root, power_ok, gpu_ok = int(root), int(power_ok), int(gpu_ok)
interval_s = float(interval) / 1000.0

def load_samples(path):
    rows = []
    for line in open(path, encoding='utf-8', errors='ignore'):
        line = line.strip().replace('\r', '')
        if not line: continue
        if line.startswith('SAMPLER_DONE'): continue
        if line.startswith('MARK_'):
            m = line.split(); rows.append(('marker', m[0], int(m[1]))); continue
        p = line.split()
        if len(p) < 13: continue   # 非 root 13 字段; root 时 13+3=16
        ts = int(p[0])
        f0, f3, f7 = int(p[1]), int(p[2]), int(p[3])
        tc, tb = int(p[4]), int(p[5])
        st = [int(x) for x in p[6:13]]               # /proc/stat: user nice system idle iowait irq softirq
        extra = [int(x) for x in p[13:]] if len(p) > 13 else []
        rows.append(('sample', ts, (f0, f3, f7, tc, tb, st, extra)))
    return rows

rows  = load_samples(os.path.join(rundir, 'soc_samples.csv'))
idle  = [r for r in load_samples(os.path.join(rundir, 'soc_idle.csv')) if r[0] == 'sample']
mark  = {row[1]: row[2] for row in rows if row[0] == 'marker'}
samp  = [r for r in rows if r[0] == 'sample']
t0, t1 = mark.get('MARK_INFERENCE_START'), mark.get('MARK_INFERENCE_END')
win = [r for r in samp if (t0 is None or r[1] >= t0) and (t1 is None or r[1] <= t1)]

def stats(rows, idx, scale=1.0):
    v = [r[2][idx]*scale for r in rows]
    return (float(sum(v)/len(v)), float(max(v)), float(min(v))) if v else (0.0, 0.0, 0.0)

def freq_hist(rows, idx):
    d = {}
    for r in rows: d[r[2][idx]] = d.get(r[2][idx], 0) + 1
    return d

def cpu_busy(rows):
    out, prev = [], None
    for r in rows:
        st = r[2][5]; tot = sum(st)
        if prev:
            dt = tot - prev[0]
            if dt > 0: out.append((r[1], 100.0*(dt - (st[3]-prev[1]))/dt))
        prev = (tot, st[3])
    return out

def temp_stats(rows, idx):
    v = [r[2][idx]/1000.0 for r in rows]   # 读数 0.001°C
    return (round(sum(v)/len(v),2), round(max(v),2), round(min(v),2), round(v[-1]-v[0],2)) if v else (0,0,0,0)

S = {}
S['window_ms'] = round((t1-t0)/1e6, 1) if (t0 and t1) else None
S['n_samples'] = len(win); S['n_idle_samples'] = len(idle)

def frec(idx):
    if not win: return None
    avg, pk, mn = stats(win, idx, 1/1000.0)   # kHz->MHz
    top = sorted(freq_hist(win, idx).items(), key=lambda kv: -kv[1])[:3]
    return {'avg_mhz': round(avg,1), 'peak_mhz': round(pk,1), 'min_mhz': round(mn,1),
            'top_freqs_mhz': {int(k/1000): round(cnt/len(win),3) for k, cnt in top}}

S['cpu_cluster_freq_mhz'] = {
    'little(cpu0)': frec(0), 'mid(cpu3)': frec(1), 'big(cpu7)': frec(2)}

cpu_t, batt_t = temp_stats(win, 3), temp_stats(win, 4)
S['temp_c'] = {'cpu_avg': cpu_t[0], 'cpu_peak': cpu_t[1], 'cpu_delta': cpu_t[3],
               'batt_avg': batt_t[0], 'batt_peak': batt_t[1], 'batt_delta': batt_t[3]}

busy = cpu_busy(win)
S['cpu_busy_pct'] = {'avg': round(sum(b for _, b in busy)/len(busy),1),
                     'peak': round(max(b for _, b in busy),1)} if busy else None

# 相邻样本实际时间差 (高负载下采样实际频率会低于配置, 积分用实测 Δt 更准)
def dt_pairs(rows):
    out = []
    for i in range(len(rows)-1):
        dt = (rows[i+1][1] - rows[i][1]) / 1e9
        if dt > 0: out.append((rows[i], dt))
    return out

dwin = dt_pairs(win)
tot_dt = sum(dt for _, dt in dwin)
S['actual_sample_hz'] = round(len(dwin)/tot_dt, 1) if tot_dt else None

# 相对能耗代理 (免 root): Σ(3*f0 + 4*f3 + 1*f7)*Δt  kHz·s (按簇核数加权, 实测Δt)
S['energy_proxy_khz_s'] = round(sum((3*r[2][0] + 4*r[2][1] + 1*r[2][2])*dt for r, dt in dwin), 0)

if root and power_ok and dwin:
    pw_w = [(r[2][6][0]/1e6, dt) for r, dt in dwin]    # power_now uW -> W
    energy_j = sum(pw*dt for pw, dt in pw_w)
    avg = energy_j/tot_dt if tot_dt else 0.0
    idle_d = dt_pairs(idle)
    idle_tot = sum(dt for _, dt in idle_d)
    idle_e = sum((r[2][6][0]/1e6)*dt for r, dt in idle_d)
    base = idle_e/idle_tot if idle_tot else 0.0
    S['power_w'] = {'avg': round(avg,3), 'peak': round(max(pw for pw, _ in pw_w),3),
                    'min': round(min(pw for pw, _ in pw_w),3),
                    'energy_j': round(energy_j,2), 'idle_baseline_w': round(base,3),
                    'net_avg_w': round(avg-base,3), 'duration_s': round(tot_dt,2)}
    if gpu_ok:
        g  = [r[2][6][1]/1e6 for r in win]             # Hz -> MHz
        gb = [r[2][6][2] for r in win]
        S['gpu'] = {'freq_mhz': {'avg': round(sum(g)/len(g),1), 'peak': round(max(g),1)},
                    'busy_pct': {'avg': round(sum(gb)/len(gb),1), 'peak': round(max(gb),1)}}

# TTFT / TPOT 解析
raw = open(os.path.join(rundir, 'inference.log'), encoding='utf-8', errors='ignore').read().replace('\r','')
def val(pat):
    m = re.search(pat, raw); return m.group(1) if m else None
pe_ms = val(r'prompt eval time =\s+([0-9.]+) ms')
et_ms = val(r'llama_perf_context_print:\s+eval time =\s+([0-9.]+) ms')
er    = val(r'eval time =\s+[0-9.]+ ms\s*/\s*([0-9]+) runs')
tpot = ttft = tps = None
if et_ms and er and float(er) > 0:
    tpot = float(et_ms)/float(er); tps = 1000.0/tpot
    ttft = (float(pe_ms) + tpot) if pe_ms else None
else:
    m = re.search(r'"tg_ts":\s*([0-9.]+)', raw)
    if m: tps = float(m.group(1)); tpot = 1000.0/tps
S['ttft_ms'] = round(ttft,1) if ttft else None
S['tpot_ms'] = round(tpot,2) if tpot else None
S['tg_tokens_per_s'] = round(tps,2) if tps else None
S['model'] = model
S['root'] = bool(root); S['power_measured'] = bool(root and power_ok)

with open(os.path.join(rundir, 'summary.json'), 'w', encoding='utf-8') as f:
    json.dump(S, f, ensure_ascii=False, indent=2)

hdr = ['device','model','n_samples','TTFT_ms','TPOT_ms','tg_tps',
       'little_avg_mhz','little_peak_mhz','mid_avg_mhz','big_avg_mhz',
       'cpu_busy_avg_pct','temp_cpu_peak_c','energy_proxy_khz_s',
       'power_avg_w','power_peak_w','energy_j','idle_base_w','net_avg_w','root']
freq_of = lambda clu, key: ((S.get('cpu_cluster_freq_mhz') or {}).get(clu) or {}).get(key)
row = [dev, model, S['n_samples'], S['ttft_ms'], S['tpot_ms'], S['tg_tokens_per_s'],
       freq_of('little(cpu0)', 'avg_mhz'), freq_of('little(cpu0)', 'peak_mhz'),
       freq_of('mid(cpu3)', 'avg_mhz'), freq_of('big(cpu7)', 'avg_mhz'),
       (S.get('cpu_busy_pct') or {}).get('avg'),
       (S.get('temp_c') or {}).get('cpu_peak'),
       S.get('energy_proxy_khz_s'),
       (S.get('power_w') or {}).get('avg'), (S.get('power_w') or {}).get('peak'),
       (S.get('power_w') or {}).get('energy_j'), (S.get('power_w') or {}).get('idle_baseline_w'),
       (S.get('power_w') or {}).get('net_avg_w'), S['root']]
with open(csv_all, 'a', newline='') as f:
    w = csv.writer(f)
    if os.path.getsize(csv_all) == 0: w.writerow(hdr)
    w.writerow([x if x is not None else '' for x in row])

print(json.dumps(S, ensure_ascii=False, indent=2))
PY

echo ""
echo "========== 汇总 (推理窗口内) =========="
[ -f "$RUN_DIR/summary.json" ] && "$PY" -c "import json,sys; print(json.dumps(json.load(open(sys.argv[1])), ensure_ascii=False, indent=2))" "$RUN_DIR/summary.json" || true
echo ""
echo "完整输出: $RUN_DIR/  (soc_samples.csv / soc_idle.csv / inference.log / summary.json)"
echo "历史汇总: $CSV_ALL"

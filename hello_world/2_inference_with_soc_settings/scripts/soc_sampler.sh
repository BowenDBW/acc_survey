#!/system/bin/sh
# ============================================================
# soc_sampler.sh — 手机端采样循环 (POSIX sh / toybox, 推送后运行)
#
# 用法:  sh soc_sampler.sh <out_file> <stop_file> <root_flag> <sleep_sec>
#
# 每 <sleep_sec> 秒追加一行到 <out_file>, 直到 <stop_file> 出现:
#   时间(device epoch ns) cpu0_freq cpu3_freq cpu7_freq \
#   temp_cpuss temp_battery /proc/stat(cpu 行的 user nice system idle iowait irq softirq)
#   若 root_flag=1, 再追加: power_now(uW) gpuclk(Hz) gpu_busy(%)
#
# 采样核: cpu0=小核簇(A510) / cpu3=中核簇(A715) / cpu7=大核簇(X3), 各簇取代表核。
# 温度: 按 type 动态定位 cpuss-0 与 battery。
# ============================================================
OUT="$1"; STOP="$2"; ROOT="$3"; SLEEP="$4"
[ -z "$SLEEP" ] && SLEEP=0.1

# ---- 按 type 查找 thermal zone 路径 (只在启动时查一次) ----
find_tz() {
  for f in /sys/class/thermal/thermal_zone*; do
    [ "$(cat "$f/type" 2>/dev/null)" = "$1" ] && { echo "$f"; return; }
  done
  echo ""
}
TZ_CPU="$(find_tz cpuss-0)"; [ -z "$TZ_CPU" ] && TZ_CPU="$(find_tz cpu-1-0)"
TZ_BAT="$(find_tz battery)"

C0=/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
C3=/sys/devices/system/cpu/cpu3/cpufreq/scaling_cur_freq
C7=/sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq

# 清理可能残留的停止标记
rm -f "$STOP"

while [ ! -f "$STOP" ]; do
  F0=$(cat "$C0" 2>/dev/null)
  F3=$(cat "$C3" 2>/dev/null)
  F7=$(cat "$C7" 2>/dev/null)
  PS=$(awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8}' /proc/stat)
  TC=$(cat "$TZ_CPU/temp" 2>/dev/null)
  TB=$(cat "$TZ_BAT/temp" 2>/dev/null)
  LINE="$(date +%s%N) $F0 $F3 $F7 $TC $TB $PS"
  if [ "$ROOT" = "1" ]; then
    PW=$(cat /sys/class/power_supply/battery/power_now 2>/dev/null)
    GV=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null)
    GB=$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null)
    LINE="$LINE $PW $GV $GB"
  fi
  echo "$LINE" >> "$OUT"
  sleep "$SLEEP"
done
echo "SAMPLER_DONE $(date +%s%N)" >> "$OUT"

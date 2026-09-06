#!/system/bin/sh
# ============================================================
# soc_sampler_remote.sh — 手机端 SoC 遥测采样（无 root，纯 cat）
#
# 可读项（2026-09-05 实测 小米13 Pro / SM8550 / Android16 / shell）：
#   CPU 频率 : /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq
#   CPU 占用 : /proc/stat 单次读取、两次采样差
#   GPU 温度 : thermal_zone 里 gpuss-0..7（毫°C）
#   NPU 温度 : thermal_zone 里 nspss-0..3（NSP=NPU 子系统）
#   CPU 温度 : cpu-1-*（大核簇）与 cpuss-*（簇子系统）
#   GPU 限频 : cooling_device37 (gpu cur/max)
#   NPU 限频 : cooling_device39 (cdsp cur/max) / 38 (cdsp_hw)
#
# root-only（勿试）：kgsl gpuclk/gpu_busy_percentage、devfreq cur_freq、
#   battery power_now/current_now
#
# 性能：MIUI 内核读 thermal_zone 慢(30-55ms/个)。启动时一次性按前缀分类建索引，
#   采样只直读相关 zone。温度单位毫°C，输出前 /1000。
#
# 用法: sh soc_sampler_remote.sh <count> <interval_s>
#   sh soc_sampler_remote.sh 10 1
# ============================================================
count=${1:-5}
interval=${2:-1}

# ---- 启动建索引：按前缀分类 zone 路径 ----
gpuss=""; nspss=""; cpuss=""; cpu_big=""
for z in /sys/class/thermal/thermal_zone[0-9]*; do
  n=$(cat "$z/type" 2>/dev/null)
  case "$n" in
    gpuss-*)  gpuss="$gpuss $z" ;;
    nspss-*)  nspss="$nspss $z" ;;
    cpuss-*)  cpuss="$cpuss $z" ;;
    cpu-1-*)  cpu_big="$cpu_big $z" ;;
  esac
done

max_temp_c() {   # $1 = space-separated zone paths → 输出该组最大 °C
  m=0
  for z in $1; do
    t=$(cat "$z/temp" 2>/dev/null)
    [ -n "$t" ] && [ "$t" -gt "$m" ] 2>/dev/null && m=$t
  done
  echo $((m/1000))
}

sample() {
  # --- CPU 占用（单次读 /proc/stat，total 与 idle 来自同一行）---
  read t1 i1 <<EOF
$(awk '/^cpu / {print $2+$3+$4+$5+$6+$7+$8, $5+$6}' /proc/stat)
EOF
  if [ -n "$t0" ] && [ -n "$i0" ] && [ -n "$t1" ] && [ -n "$i1" ]; then
    tot=$((t1-t0)); idle=$((i1-i0))
    if [ "$tot" -gt 0 ]; then
      util=$((100*(tot-idle)/tot))
      [ "$util" -lt 0 ] && util=0; [ "$util" -gt 100 ] && util=100
    else util=0; fi
  else util=-1; fi
  t0=$t1; i0=$i1

  # --- CPU 频率（8 核）---
  freq=""; i=0
  while [ $i -lt 8 ]; do
    f=$(cat /sys/devices/system/cpu/cpu$i/cpufreq/scaling_cur_freq 2>/dev/null)
    freq="$freq c$i=$((f/1000))"
    i=$((i+1))
  done

  gpu_c=$(max_temp_c "$gpuss")
  npu_c=$(max_temp_c "$nspss")
  cpu_c=$(max_temp_c "$cpu_big")

  echo "[$(date +%H:%M:%S)] CPUutil=${util}%"
  echo "  freq:${freq}"
  echo "  TEMP : GPU=${gpu_c}C  NPU=${npu_c}C  CPUmax=${cpu_c}C"
  echo "  COOL : gpu=$(cat /sys/class/thermal/cooling_device37/cur_state 2>/dev/null)/$(cat /sys/class/thermal/cooling_device37/max_state 2>/dev/null)"
  echo "         cdsp=$(cat /sys/class/thermal/cooling_device39/cur_state 2>/dev/null)/7  cdsp_hw=$(cat /sys/class/thermal/cooling_device38/cur_state 2>/dev/null)/1"
}

# 预读基线，让首个样本也能算 CPU 占用
read t0 i0 <<EOF
$(awk '/^cpu / {print $2+$3+$4+$5+$6+$7+$8, $5+$6}' /proc/stat)
EOF
j=0
while [ $j -lt $count ]; do
  sample
  j=$((j+1))
  [ $j -lt $count ] && sleep $interval
done

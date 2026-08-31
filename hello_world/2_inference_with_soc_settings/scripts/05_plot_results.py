#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
05_plot_results.py — 把某次 run 的采样结果画成图 (需要 matplotlib)

用法:
  python scripts/05_plot_results.py <run_dir> [输出png路径]

<run_dir> 下需要 soc_samples.csv (含 MARK_INFERENCE_START/END) 与 soc_idle.csv。
输出: <run_dir>/soc_plot.png  (默认)
图中用竖线标出推理窗口 [START, END]。
"""
import sys, os

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
except ImportError:
    print("[!] 未安装 matplotlib, 跳过画图 (可: pip install matplotlib)")
    sys.exit(0)

def load(path):
    t, f0, f3, f7, tc, tb, busy, pw = [], [], [], [], [], [], [], []
    prev = None
    for line in open(path, encoding='utf-8', errors='ignore'):
        line = line.strip().replace('\r', '')
        if not line or line.startswith('SAMPLER_DONE'): continue
        if line.startswith('MARK_'):
            continue
        p = line.split()
        if len(p) < 13: continue   # 非 root 13 字段; root 时 13+3=16
        ts = int(p[0]); st = [int(x) for x in p[6:13]]
        tot = sum(st)
        if prev:
            dt = tot - prev[0]
            if dt > 0: busy.append((ts, 100.0*(dt - (st[3]-prev[1]))/dt))
        prev = (tot, st[3])
        t.append(ts); f0.append(int(p[1])/1e3); f3.append(int(p[2])/1e3); f7.append(int(p[3])/1e3)
        tc.append(int(p[4])/1e3); tb.append(int(p[5])/1e3)
        if len(p) > 13 and p[13].isdigit(): pw.append((ts, int(p[13])/1e6))
    return (t, f0, f3, f7, tc, tb, busy, pw)

def main():
    run = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(run, 'soc_plot.png')
    t, f0, f3, f7, tc, tb, busy, pw = load(os.path.join(run, 'soc_samples.csv'))
    # 推理窗口标记
    marks = []
    for line in open(os.path.join(run, 'soc_samples.csv'), encoding='utf-8', errors='ignore'):
        if line.startswith('MARK_'):
            m = line.split(); marks.append((m[0], int(m[1])/1e9))
    base = min(marks[0][1], (t[0]/1e9 if t else 0))

    fig, axs = plt.subplots(4, 1, figsize=(12, 11), sharex=True)
    axs[0].plot([x/1e9-base for x in t], f0, label='little cpu0 (A510)', lw=1)
    axs[0].plot([x/1e9-base for x in t], f3, label='mid cpu3 (A715)', lw=1)
    axs[0].plot([x/1e9-base for x in t], f7, label='big cpu7 (X3)', lw=1)
    axs[0].set_ylabel('CPU freq (MHz)'); axs[0].legend(); axs[0].grid(alpha=.3)
    axs[0].set_title(f'SoC monitoring — {os.path.basename(run)}')

    axs[1].plot([x/1e9-base for x in t], tc, label='cpuss temp', lw=1)
    axs[1].plot([x/1e9-base for x in t], tb, label='battery temp', lw=1)
    axs[1].set_ylabel('temp (C)'); axs[1].legend(); axs[1].grid(alpha=.3)

    if busy:
        axs[2].plot([x/1e9-base for x, y in busy], [y for x, y in busy], color='tab:green', lw=1)
        axs[2].set_ylabel('CPU busy (%)'); axs[2].set_ylim(0, 105); axs[2].grid(alpha=.3)
    else:
        axs[2].text(.5, .5, 'no busy data', ha='center'); axs[2].set_ylabel('CPU busy (%)')

    if pw:
        axs[3].plot([x/1e9-base for x, y in pw], [y for x, y in pw], color='tab:red', lw=1)
        axs[3].set_ylabel('power (W)'); axs[3].grid(alpha=.3)
    else:
        axs[3].text(.5, .5, 'power not measured (root needed)', ha='center')
        axs[3].set_ylabel('power (W)')

    for ax in axs:
        for name, ts in marks:
            ax.axvline(ts/1e9-base, color='tab:orange', ls='--', lw=1, alpha=.8,
                       label=('inference window' if name == 'MARK_INFERENCE_START' else None))
    axs[0].legend(loc='upper right')
    axs[3].set_xlabel('time (s)')
    fig.tight_layout()
    fig.savefig(out, dpi=110)
    print(f"[plot] 已保存: {out}")

if __name__ == '__main__':
    main()

# -*- coding: utf-8 -*-
"""分析 tools/drift_raw.csv：漂移特征提取 + 模型拟合。
输出：分段漂移速率、指数模型拟合参数、稳态噪声、温度灵敏度、异常跳变。"""
import sys
import numpy as np
import pandas as pd

LSB_PER_G = 1425.0   # scale.h 标定: 10g -> 14250 LSB，即 1g ≈ 1425 LSB（用于把 raw 漂移换算成克）

df = pd.read_csv('tools/drift_raw.csv')
t = df['elapsed_s'].to_numpy()
v = df['value'].to_numpy()
tmp = df['temp'].to_numpy()
print(f'样本: {len(df)}, 时长: {t[-1]:.1f}s, 温度范围: {tmp.min():.1f}..{tmp.max():.1f} C')

# ---- 1. 分段漂移速率 ----
def seg(ta, tb):
    m = (t >= ta) & (t <= tb)
    if m.sum() < 2:
        return None
    dt = t[m][-1] - t[m][0]
    dv = v[m][-1] - v[m][0]
    return dv, dt, dv / dt

print('\n== 分段漂移 ==')
for (a, b) in [(0, 30), (30, 135), (135, 300), (300, 600), (600, 900)]:
    r = seg(a, b)
    if r:
        print(f'  t={a:>4}..{b:>4}s: 漂移 {r[0]:+9.0f} LSB ({r[0]/LSB_PER_G:+.4f} g)  平均速率 {r[2]:+8.1f} LSB/s ({r[2]/LSB_PER_G:+.5f} g/s)')

# ---- 2. 指数 + 线性拟合 raw(t) = A*exp(-t/tau) + B*t + C（前 300s）----
print('\n== 指数模型拟合 (前 300s) ==')
m300 = t <= 300
tt, vv = t[m300], v[m300]
try:
    from scipy.optimize import curve_fit
    def model(x, A, tau, B, C):
        return A * np.exp(-x / tau) + B * x + C
    p0 = [8000.0, 30.0, 2.0, vv[0] - 8000.0]
    popt, _ = curve_fit(model, tt, vv, p0=p0, maxfev=20000)
    A, tau, B, C = popt
    print(f'  raw(t) = {A:.0f}*exp(-t/{tau:.1f}s) + {B:.2f}*t + {C:.0f}')
    print(f'  初始漂移幅度 A = {A:.0f} LSB ({A/LSB_PER_G:.3f} g), 时间常数 tau = {tau:.1f} s')
    print(f'  线性残留速率 B = {B:.2f} LSB/s ({B/LSB_PER_G:.5f} g/s)')
    # 残差
    resid = vv - model(tt, *popt)
    print(f'  拟合残差 std = {resid.std():.0f} LSB ({resid.std()/LSB_PER_G:.4f} g)')
except ImportError:
    print('  scipy 不可用，跳过曲线拟合')

# ---- 3. 稳态噪声（后 300s）----
m_end = t >= t[-1] - 300
v_end = v[m_end]
print(f'\n== 稳态噪声 (后 300s) ==')
print(f'  raw std = {v_end.std():.0f} LSB ({v_end.std()/LSB_PER_G:.4f} g)')
print(f'  raw p-p = {v_end.max()-v_end.min():.0f} LSB ({ (v_end.max()-v_end.min())/LSB_PER_G:.4f} g)')

# ---- 4. 温度灵敏度（后 600s 线性回归 raw vs temp）----
print('\n== 温度灵敏度 (后 600s) ==')
m6 = t >= t[-1] - 600
tt6, vv6, tmp6 = t[m6], v[m6], tmp[m6]
# 去掉趋势: 先对 t 回归取残差
A_t = np.polyfit(tt6, vv6, 1)
resid6 = vv6 - np.polyval(A_t, tt6)
A_tmp = np.polyfit(tmp6, resid6, 1)
print(f'  时间趋势: {A_t[0]:.2f} LSB/s')
print(f'  温度灵敏度: {A_tmp[0]:.0f} LSB/C ({A_tmp[0]/LSB_PER_G:.4f} g/C)')

# ---- 5. 异常跳变检测（相邻样本差分速率超过 200 LSB/s）----
print('\n== 异常跳变 (相邻差分 > 200 LSB/s) ==')
dt_s = np.diff(t)
dv_s = np.diff(v)
rate = np.abs(dv_s / dt_s)
jumps = np.where(rate > 200.0)[0]
if len(jumps) == 0:
    print('  无')
else:
    # 合并相邻
    groups = []
    start = prev = jumps[0]
    for j in jumps[1:]:
        if j - prev > 3:
            groups.append((start, prev))
            start = j
        prev = j
    groups.append((start, prev))
    for g in groups:
        i0, i1 = g[0], g[1]
        print(f'  t={t[i0]:.0f}..{t[i1]:.0f}s  raw {v[i0]:.0f} -> {v[i1]:.0f}  峰值速率 {rate[i0:i1+1].max():.0f} LSB/s ({rate[i0:i1+1].max()/LSB_PER_G:.3f} g/s)')

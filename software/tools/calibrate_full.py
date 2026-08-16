# -*- coding: utf-8 -*-
"""全量程标定分析：0~500g 递增/回程数据。
数据来自 Desktop/weight_records_20260816_152035.csv（15:14-15:20 采集）。
当前标定 SCALE_LSB_TO_G = 10/11950。
输出：误差率曲线、滞后、单系数 vs 二次多项式拟合残差、建议新标定。
"""
import numpy as np

# 真实重量 (g)
x_true = np.array([50, 100, 150, 200, 250, 300, 350, 400, 450, 500], dtype=float)
# 递增显示值（CSV：49.87/49.43/50.16 取平均 49.82；其余取对应行）
y_up = np.array([49.82, 99.78, 149.60, 199.64, 249.31, 299.22,
                 348.86, 398.50, 448.42, 497.64], dtype=float)
# 回程显示值（CSV）
y_down = np.array([50.54, 100.45, 150.00, 199.83, 249.45, 299.56,
                   349.34, 398.93, 448.71, 497.64], dtype=float)  # 注: 回程从450开始, 对齐时末点用500的递增值
y_down = np.array([50.54, 100.45, 150.00, 199.83, 249.45, 299.56,
                   349.34, 398.93, 448.71], dtype=float)          # 只有 50..450 有回程
x_down = x_true[:9]

print('== 误差率（递增，显示/真实-1）==')
for x, y in zip(x_true, y_up):
    print(f'  {x:5.0f} g: 显示 {y:8.2f}  误差 {y-x:+7.2f} g ({(y-x)/x*100:+.2f}%)')

print('\n== 滞后（回程-递增，同重量）==')
hys = y_down - y_up[:9]
for x, h in zip(x_down, hys):
    print(f'  {x:5.0f} g: {h:+.2f} g')
print(f'  平均滞后: {hys.mean():+.2f} g ({hys.mean()/500*100:.2f}% FS)')

# ---- 拟合: 显示 = k * 真实（过原点最小二乘）----
k = np.sum(x_true * y_up) / np.sum(x_true * x_true)
resid = y_up - k * x_true
print(f'\n== 单系数拟合 (递增) ==')
print(f'  k = 显示/真实 = {k:.5f}  最大残差 {np.abs(resid).max():+.3f} g')
print(f'  新 SCALE_LSB_TO_G = (10/11950)/{k:.5f} = {(10/11950)/k:.9f}  (10/{10/((10/11950)/k):.1f})')

# ---- 拟合: 显示 = a*真实 + b*真实^2（二次，过原点）----
A = np.vstack([x_true, x_true**2]).T
coef, *_ = np.linalg.lstsq(A, y_up, rcond=None)
resid2 = y_up - (coef[0]*x_true + coef[1]*x_true**2)
print(f'\n== 二次拟合 显示 = {coef[0]:.4f}*x {coef[1]:+.2e}*x^2 (递增) ==')
print(f'  最大残差 {np.abs(resid2).max():+.3f} g')
print('  校正公式: 真实 = raw*LSB/(a + b*真实) 需要迭代, 或直接对 raw 用二次:')

# raw 域校正: 显示g = raw*LSB; 真实 = (显示 + c2*显示^2)/a 形式
# 用真实 = p1*显示 + p2*显示^2 反拟合
A2 = np.vstack([y_up, y_up**2]).T
p, *_ = np.linalg.lstsq(A2, x_true, rcond=None)
resid3 = x_true - (p[0]*y_up + p[1]*y_up**2)
print(f'  真实 = {p[0]:.4f}*显示 {p[1]:+.2e}*显示^2  最大残差 {np.abs(resid3).max():.3f} g')

# 递增+回程平均的二次拟合（抵消滞后）
y_avg = np.concatenate([y_up, y_down])
x_avg = np.concatenate([x_true, x_down])
A3 = np.vstack([y_avg, y_avg**2]).T
p3, *_ = np.linalg.lstsq(A3, x_avg, rcond=None)
resid4 = x_avg - (p3[0]*y_avg + p3[1]*y_avg**2)
print(f'\n== 二次拟合（递增+回程平均, 19 点）==')
print(f'  真实 = {p3[0]:.4f}*显示 {p3[1]:+.2e}*显示^2  最大残差 {np.abs(resid4).max():.3f} g')

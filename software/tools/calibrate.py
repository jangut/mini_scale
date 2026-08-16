# -*- coding: utf-8 -*-
"""标定计算：根据桌面校准数据（0~200g，每级 10g 砝码，置零后逐级记录显示值）
计算真实 SCALE_LSB_TO_G。
假设：砝码每级 10g（0,10,...,200g），共 20 级，与数据点数（20）吻合。
显示值 = raw_delta * SCALE_LSB_TO_G_old，真实 = raw_delta * SCALE_LSB_TO_G_new
=> SCALE_LSB_TO_G_new = SCALE_LSB_TO_G_old * (真实/显示)，用线性回归斜率 k=显示/真实。
"""
import csv
import os

OLD = 10.0 / 14250.0          # scale.h 当前标定
CSV = os.path.join(os.path.expanduser('~'), 'Desktop',
                   'weight_records_20260816_144541.csv')

rows = []
with open(CSV, newline='') as f:
    for r in csv.DictReader(f):
        rows.append(float(r['weight_g']))

# 置零（0.000）之后的数据点 = 逐级加砝码的显示值。
# 注意文件开头 12.470 是置零前的读数（秤上原有偏置），必须排除。
zero_idx = None
for i, v in enumerate(rows):
    if abs(v) < 0.01:
        zero_idx = i
assert zero_idx is not None, '未找到置零点(0.000)'
data = [v for v in rows[zero_idx + 1:] if v > 0.01]
n = len(data)
print(f'数据点: {n} 个（置零点之后）')

# 假设每级 10g：真实重量 = 10, 20, ..., 10*n
x_true = [10.0 * (i + 1) for i in range(n)]

# 线性回归 y(显示) = k * x(真实)，过原点
sxy = sum(x * y for x, y in zip(x_true, data))
sxx = sum(x * x for x in x_true)
k = sxy / sxx

print(f'拟合斜率 k = 显示/真实 = {k:.5f}')
for i in (0, 5, 10, 19):
    if i < n:
        print(f'  真实 {x_true[i]:4.0f} g -> 显示 {data[i]:8.3f} g (比例 {data[i]/x_true[i]:.4f})')
print(f'  显示偏小 {(1-k)*100:.1f}%')

new = OLD / k
print(f'\n旧标定 SCALE_LSB_TO_G = 10/14250 = {OLD:.9f} g/LSB')
print(f'新标定 SCALE_LSB_TO_G = {new:.9f} g/LSB')
print(f'等价写法: 10/{10.0/new:.1f}')
print(f'对应 1g = {1.0/new:.1f} LSB')

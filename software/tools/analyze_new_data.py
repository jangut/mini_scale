# -*- coding: utf-8 -*-
"""分析 weight_records_20260817_011234.csv：GUI 采集的 0-500g 递增数据。"""
import csv, os

CSV = os.path.join(os.path.expanduser('~'), 'Desktop',
                   'weight_records_20260817_011234.csv')
rows = []
with open(CSV, newline='') as f:
    for r in csv.DictReader(f):
        rows.append(float(r['weight_g']))

# 用户加砝码序列：0,10,20,...,200, 220,210,230,240,...,500
true_w = [0]
for g in range(10, 210, 10):
    true_w.append(g)
true_w += [220, 210, 230]
for g in range(240, 510, 10):
    true_w.append(g)

print(f'数据点 {len(rows)} 个, 真实序列 {len(true_w)} 个')
print(f'{"真实g":>6} {"显示g":>9} {"误差g":>7} {"误差%":>8}')
for t, y in zip(true_w, rows):
    e = y - t
    pct = 100.0 * e / t if t else 0.0
    print(f'{t:6d} {y:9.2f} {e:+7.2f} {pct:+7.2f}%')

# 常规段统计（排除 220/210/230 段）
skip = {220, 210, 230}
errs = [(t, y - t) for t, y in zip(true_w, rows) if t not in skip and t > 0]
maxe = max(errs, key=lambda x: abs(x[1]))
print(f'\n常规段最大误差: {maxe[0]}g 处 {maxe[1]:+.2f}g')

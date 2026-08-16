# -*- coding: utf-8 -*-
"""
零漂移监听工具（上位机）
========================
监听 STM32 秤的串口输出，长时间记录空秤（0 点）数据，实时统计漂移
速率与幅度，并输出 CSV 供分析 —— 用于针对性地抑制 0 点漂移
（调 auto-zero 参数 / 暖机检测阈值）。

数据格式（固件 App_UartSend，9600 8N1）:
  正常模式  weight,temp\\n    e.g. "0.00,25.6"   （重量 g，温度 °C）
  校准模式  raw,temp\\n        e.g. "14250,25.6"  （滤波后原始 ADC 码）

注意:
  - raw 是滤波后的原始 ADC 码，未经过 tare / auto-zero 修正，
    能反映真实漂移 —— 观察漂移请用校准模式（长按 KEY1 进入
    Periph Test 模式，其 UART 输出 raw,temp）。
  - weight 模式显示值会被 auto-zero 拉回 0，漂移被掩盖。

用法:
  python zero_drift_monitor.py --port COM5 --baud 9600 --duration 600 --csv drift.csv
  （--duration 0 = 一直监听直到 Ctrl+C）
"""
import argparse
import csv
import time
import sys
import serial

LSB_PER_G = 1425.0   # scale.h 标定: 10g -> 14250 LSB，即 1g ≈ 1425 LSB（用于把 raw 漂移换算成克）


def parse_line(line):
    """解析一行 -> (kind, value, temp)；kind 为 'weight'/'raw'/'warmup'/'sleep'，失败返回 None"""
    line = line.strip()
    if line.startswith('WARMUP:'):
        try:
            return ('warmup', int(line[7:]), 0.0)
        except ValueError:
            return None
    if line == 'SLEEP':
        return ('sleep', 0, 0.0)
    if not line or ',' not in line:
        return None
    parts = line.split(',')
    if len(parts) < 2:
        return None
    try:
        temp = float(parts[1])
        if '.' in parts[0]:
            return ('weight', float(parts[0]), temp)
        return ('raw', int(parts[0]), temp)
    except ValueError:
        return None


def fmt_value(kind, val):
    if kind == 'raw':
        return f'{val} LSB ({val / LSB_PER_G:+.3f} g)'
    return f'{val:+.3f} g'


def main():
    ap = argparse.ArgumentParser(description='零漂移监听：记录空秤串口数据并统计漂移')
    ap.add_argument('--port', default='COM5', help='串口号')
    ap.add_argument('--baud', type=int, default=9600, help='波特率')
    ap.add_argument('--duration', type=float, default=0.0,
                    help='监听秒数（0 = 直到 Ctrl+C）')
    ap.add_argument('--csv', default='zero_drift.csv', help='CSV 输出文件路径')
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as e:
        print(f'无法打开 {args.port}: {e}')
        sys.exit(1)
    # 不触发 ESP32-S3 复位（若经 ESP32 板 CH343 透传，DTR 复位会打印 boot 日志干扰）
    ser.dtr = False
    ser.rts = False

    print(f'监听 {args.port} @ {args.baud}  输出 CSV: {args.csv}')
    print(f'{"时间(s)":>10} {"数值":>24} {"温度":>7} {"漂移速率":>12}')
    print('-' * 64)

    t0 = time.time()
    first = None          # (kind, value, temp)
    last = None
    n = 0
    window = []           # 最近 10 秒的 (elapsed, value, kind)
    max_rate = (0.0, None)   # (速率, 出现时刻)
    last_report = 0.0

    with open(args.csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['elapsed_s', 'value', 'temp', 'kind'])
        try:
            while args.duration <= 0 or time.time() - t0 < args.duration:
                raw = ser.readline()
                if not raw:
                    continue
                parsed = parse_line(raw.decode('latin-1'))
                if parsed is None:
                    continue
                kind, val, temp = parsed
                now = time.time() - t0
                # 状态行（暖机/睡眠）只打印，不写入数据 CSV、不计入统计
                if kind in ('warmup', 'sleep'):
                    if now - last_report >= 1.0 or kind == 'sleep':
                        last_report = now
                        tag = f'暖机剩余 {val} s' if kind == 'warmup' else '设备睡眠 (按 KEY 唤醒)'
                        print(f'{now:10.1f} {tag:>40}')
                    continue
                if first is None:
                    first = (kind, val, temp)
                last = (kind, val, temp)
                n += 1
                w.writerow([f'{now:.3f}', val, temp, kind])
                f.flush()

                window.append((now, val))
                while window and now - window[0][0] > 10.0:
                    window.pop(0)

                # 每秒打印一行 + 计算最近 10 秒平均漂移速率
                if now - last_report >= 1.0:
                    last_report = now
                    rate = 0.0
                    if len(window) >= 2 and window[-1][0] - window[0][0] > 0:
                        rate = (window[-1][1] - window[0][1]) / (window[-1][0] - window[0][0])
                    if abs(rate) > abs(max_rate[0]):
                        max_rate = (rate, now)
                    rate_s = f'{rate:+.3f} {"g/s" if kind == "weight" else "LSB/s"}'
                    print(f'{now:10.1f} {fmt_value(kind, val):>24} {temp:7.1f} {rate_s:>12}')
        except KeyboardInterrupt:
            print('\n用户中断')
        finally:
            ser.close()

    # ---- 摘要 ----
    dur = time.time() - t0
    print('=' * 64)
    print(f'监听时长: {dur:.1f} s   有效样本: {n}')
    if first and last and n > 1:
        k = last[0]
        drift = last[1] - first[1]
        unit = 'g' if k == 'weight' else 'LSB'
        print(f'首值: {fmt_value(k, first[1])}  末值: {fmt_value(k, last[1])}')
        print(f'总漂移: {drift:+.3f} {unit}'
              + (f' ({drift / LSB_PER_G:+.3f} g)' if k == 'raw' else ''))
        print(f'最大漂移速率(10s 窗口): {max_rate[0]:+.3f} {unit}/s'
              + (f' ({max_rate[0] / LSB_PER_G:+.3f} g/s)' if k == 'raw' else '')
              + (f'  出现在 t={max_rate[1]:.0f}s' if max_rate[1] is not None else ''))
        if k == 'weight':
            print('提示: weight 受 auto-zero 修正，漂移可能被掩盖；')
            print('      想看真实漂移请用校准模式（长按 KEY1，输出 raw,temp）。')
    print(f'数据已保存: {args.csv}')


if __name__ == '__main__':
    main()

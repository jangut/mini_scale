"""虚拟称重传感器发送端：向指定串口持续发送 weight,temp 数据帧。

配合虚拟串口对（如 com0com / VSPD）使用：本脚本占用虚拟对的一端，
上位机连接另一端即可收到模拟的称重数据流。

用法：
    python tools/fake_scale.py --port COM3
    python tools/fake_scale.py --port COM3 --baud 57600 --rate 10

参数：
    --port   要写入的串口（虚拟对的一端）
    --baud   波特率（默认 115200，需与上位机一致）
    --rate   发送频率 Hz（默认 20）
"""
import argparse
import math
import random
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser(description="虚拟称重传感器发送端")
    ap.add_argument("--port", required=True, help="要写入的串口")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--rate", type=float, default=20.0)
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0)
    except Exception as exc:
        print(f"打开 {args.port} 失败: {exc}", file=sys.stderr)
        sys.exit(1)

    print(f"虚拟传感器运行中：{args.port} @ {args.baud}，"
          f"{args.rate:.0f} Hz，Ctrl+C 退出")
    t = 0.0
    src = dst = 0.0
    hop_at = 0.0
    hop_dur = 0.5
    next_hop = 3.0
    try:
        while True:
            dt = 1.0 / args.rate
            t += dt
            # 周期性阶跃：模拟"放上/取下重物"
            if t >= next_hop:
                src = dst
                dst = round(random.uniform(0.0, 50.0), 1)
                hop_at = t
                hop_dur = random.uniform(0.3, 0.8)
                next_hop = t + random.uniform(5.0, 9.0)
            frac = min(1.0, max(0.0, (t - hop_at) / hop_dur))
            base = src + (dst - src) * frac
            # 蠕变 + 噪声 + 温度波动
            weight = base + 0.05 * math.sin(t * 0.7) + random.gauss(0.0, 0.05)
            temp = 25.5 + 0.4 * math.sin(t / 12.0) + random.gauss(0.0, 0.03)
            line = f"{weight:.3f},{temp:.2f}\n"
            ser.write(line.encode())
            print(line, end="")
            time.sleep(dt)
    except KeyboardInterrupt:
        print("\n已退出")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
